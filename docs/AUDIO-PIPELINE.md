# Audio pipeline & DSP audit (2026-04-27)

Snapshot of how audio actually flows through the satellite today, what the
XVF3800 DSP is doing for us, and which knobs are reachable from YAML versus
which would require extending the upstream `respeaker_xvf3800` component.

## Hardware path

```
   ┌─────────────────────┐                   ┌────────────┐
   │ XVF3800 DSP         │                   │ AIC3104    │
   │ (XMOS, 4-mic array) │ ◀── reference ── │ codec/DAC  │
   │ AEC + NS + AGC      │                   └────────────┘
   │ + beamforming       │                          ▲
   └──────────┬──────────┘                          │
              │                                     │
              │ I2S primary, 48 kHz / 32-bit / stereo
              │   ch0 = AEC-processed (for ASR)
              │   ch1 = unprocessed (for wake-word)
              ▼                                     │
   ┌─────────────────────┐                          │
   │ ESP32-S3 (secondary)│ ──── speaker out ───────┘
   │  i2s_audio (patched)│
   │  micro_wake_word    │
   │  voicebuddy_satellite (BARK)
   └─────────────────────┘
```

Both directions ride the same I2S bus (LRCLK GPIO7, BCLK GPIO8). The DSP is
the I2S primary; the ESP32 is secondary. That topology is what gives the
XVF3800 its AEC reference signal "for free" — there is no separate
loopback wiring.

## What the DSP is doing right now

Firmware: `application_xvf3800_inthost-lr48-sqr-i2c-v1.0.7-release.bin`
(formatBCE upstream build).

Active by default inside the DSP, with vendor-default tuning:

- **AEC** — 4-mic acoustic echo cancellation, reference is the same
  I2S stream the codec is playing. So when the satellite is playing
  TTS or, if configured, music through `i2s_audio_speaker`, the DSP
  subtracts that from the captured mic input.
- **Noise suppression** — runs on the AEC-processed channel.
- **AGC** — auto gain on the post-processed channel.
- **Beamforming** — 12 fixed beams (30° sectors). The DSP picks the
  beam with the strongest voice energy and routes that as channel 0.
- **VNR** (voice/noise ratio) — internal estimator the DSP uses to
  decide when to talk. Readable from the host (see below).

We do **not** currently push any tuning into the DSP from the host.
Everything above runs on whatever defaults the v1.0.7 firmware ships
with.

## What's reachable from YAML today

The upstream `respeaker_xvf3800` component exposes a thin schema:

| Feature | Schema key | Type |
|---|---|---|
| Mic mute toggle / hardware button | `mute_switch:` | switch |
| DSP firmware version | `dfu_version:` | text_sensor |
| Active beam direction (0-11) | `led_beam_sensor:` | sensor |
| DFU OTA of the DSP firmware | `firmware:` | action |

Public C++ methods we can call from a YAML lambda even without a
schema entry:

- `id(respeaker).read_vnr()` — returns `uint8_t` 0-255 (linear in
  voice probability).
- `id(respeaker).read_led_beam_direction()` — returns int 0-11
  (same value `led_beam_sensor` polls).
- `id(respeaker).set_led_ring(uint32_t colors[12])` — write the
  on-board LED ring directly. Each `uint32_t` is `(R<<16)|(G<<8)|B`.
- `id(respeaker).read_mute_status()` / `write_mute_status(bool)`.

That's the entire surface today.

## What is NOT reachable from YAML

These DSP knobs all exist inside the v1.0.7 firmware (the XMOS
configuration servicer table includes them) but are **not** wired
through to YAML by the upstream component:

- Noise suppression level
- AEC enable / depth
- AGC parameters (target gain, max gain, attack/release)
- Beamforming mode (auto vs fixed direction)
- Fixed-direction override (lock the beam to one of the 12 sectors)

The C++ helper `xmos_write_bytes(resid, cmd, value, len)` exists in
`respeaker_xvf3800.cpp` and is the right vehicle, but each parameter
needs:

1. The correct RESID + CMD code from the XMOS firmware sources for
   this specific build (`inthost-lr48-sqr-i2c-v1.0.7`).
2. A new schema entry in `__init__.py` (and a Python codegen step).
3. A C++ method on `RespeakerXVF3800` that calls `xmos_write_bytes`.
4. A new entity class (Number/Select/Switch) wired to that method.

That's a separate piece of work — meaningful enough to deserve its
own branch and probably an upstream PR back to formatBCE.

## Wake-word path — the most important finding

Look at `voicebuddy-satellite-minimal.yaml:227-238`:

```yaml
micro_wake_word:
  microphone:
    microphone: i2s_mics
    channels: 1                      # ← raw, unprocessed mic
  models:
    - model: .../okay_nabu.json
      id: okay_nabu                   # uses model's manifest cutoff (0.85)
  vad:
    probability_cutoff: 0.05          # this is the VAD gate, not the WW cutoff
```

**Channel 1 is the unprocessed feed.** The XVF3800 puts AEC + NS +
AGC + beamforming output on channel 0 and the raw mic on channel 1.
mWW gets the raw signal — i.e., none of the DSP's noise-cleanup helps
the wake-word detector. That's a deliberate inheritance from
formatBCE's example, the rationale in the upstream comment is "AEC
can chew up the wake phrase."

The trade-off this makes:

- Pro: the wake phrase reaches mWW unmodified, no AEC artifacts.
- Con: every other sound also reaches mWW unmodified — including
  background music, radio, and a TV in the next room. The DSP's
  noise suppression is sitting *right there* and we're choosing not
  to use it for wake.

For the "radio in the background" scenario this is the dominant
factor. The first thing to A/B test is **flipping mWW to `channels: 0`
(AEC-cleaned)** and seeing whether real wakes still detect reliably
while spurious wakes drop.

We are not flipping it in this branch — that change deserves its own
A/B test pass with empirical data. What we do ship: a runtime
sensitivity select (Slightly / Moderately / Very) so the probability
cutoff can be tuned in the field without re-flashing.

## What this branch ships

Surface in `voicebuddy-satellite-provisioned.yaml`:

- **Voice/Noise Ratio sensor** — wraps `read_vnr()`, polled at 1 Hz,
  surfaced as 0-100 %. Diagnostic for "is the DSP actually hearing
  voice or is it confused by noise".
- **Voice beam direction sensor** — enables the upstream
  `led_beam_sensor:` entry (was previously not wired into the
  provisioned config).
- **Wake-word sensitivity select** — three presets (Slightly /
  Moderately / Very), persisted to NVS via `restore_value: true`,
  applied at boot through `on_value`. Default = "Slightly sensitive"
  (cutoff 0.85, the okay_nabu manifest default).
- **Voice direction LED switch** — opt-in (default off). When on, an
  interval automation maps the active beam onto the on-board 12-LED
  ring as a soft blue wedge. Visualises beamforming in real time.
- All four entities live in a new `tuning_group` in the web UI,
  alongside the existing provisioning fields.

What this branch does NOT ship (deferred):

- Forking `respeaker_xvf3800` to expose NS / AEC / AGC / beam-mode
  parameters as Number/Select entities.
- Hub-side parameter protocol + admin UI panel.
- mWW channel flip (`channels: 1` → `channels: 0`) — needs an A/B
  test pass.
- Hub-side wake-word re-verification with a bigger model.
