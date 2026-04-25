# How to build and flash a VoiceBuddy satellite

Step-by-step for someone new to the project. This walks you from a
boxed ReSpeaker XVF3800 to a working satellite that talks to the
VoiceBuddy hub.

> **Why ESPHome and not Arduino?** This repository is structured as
> ESPHome external components — formatBCE wrote the XVF3800 driver,
> the AIC3104 codec driver, and microWakeWord wiring as ESPHome
> components, and our `voicebuddy_satellite` is one too. ESPHome
> compiles them into a single firmware binary with one command.
> Re-doing this in the Arduino IDE would mean rewriting roughly ten
> thousand lines of C++ for no benefit. If you've never used ESPHome
> before, don't worry — it's actually simpler than the Arduino IDE
> for this hardware. Stick with the steps below.

## What you need

**Hardware**

- ReSpeaker XVF3800 USB 4-Mic Array (the dev board with the XIAO
  ESP32S3 plugged in).
- A USB-C cable that carries data, not just power. (A surprising
  number of USB-C cables are charge-only.)
- A computer running Linux, macOS, or Windows.

**Software (we'll install in step 1)**

- Docker, *or* Python 3.9+ (either is enough — pick one).
- A text editor.

**Network info**

- The IP address of your VoiceBuddy hub on your LAN.
- Your WiFi SSID and password.

You do **not** need Home Assistant. You do **not** need the Arduino
IDE. You do **not** need PlatformIO.

---

## Step 1 — Install ESPHome

You only need one of these two options. **Pick by platform:**

- **Linux / macOS:** either Option A (Docker) or Option B (pip) works.
- **Windows:** prefer **Option B (pip)**. Docker Desktop on Windows
  can't easily pass a COM port into a container without WSL2
  gymnastics, so the pip path is smoother for the first USB flash.
  After that first flash everything is OTA and Docker would work
  fine — but pip is simpler from the start.

### Option A — Docker (Linux / macOS recommended)

```sh
docker pull ghcr.io/esphome/esphome:stable
```

That's it. From here on, "run `esphome <something>`" means run it
through Docker — there's a one-liner wrapper at the bottom of this
section.

### Option B — pip (works everywhere; recommended on Windows)

#### Linux / macOS

```sh
python3 -m venv ~/.venv/esphome
source ~/.venv/esphome/bin/activate
pip install esphome
```

#### Windows (PowerShell)

First, make sure Python 3.9+ is installed. If you don't have it,
grab it from <https://www.python.org/downloads/windows/> and tick
**"Add python.exe to PATH"** in the installer.

```powershell
python -m venv $HOME\.venv\esphome
& $HOME\.venv\esphome\Scripts\Activate.ps1
pip install esphome
```

If PowerShell complains *"running scripts is disabled on this
system"* the first time you run `Activate.ps1`, run this once and
then retry:

```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
```

#### Verify either install

```sh
esphome version          # if you used pip (any platform)
# or
docker run --rm ghcr.io/esphome/esphome:stable version
```

You should see something like `Version: 2026.x.x`.

> Each new terminal needs the venv re-activated:
> `source ~/.venv/esphome/bin/activate` (Linux/macOS) or
> `& $HOME\.venv\esphome\Scripts\Activate.ps1` (Windows PowerShell).

### Docker wrapper (only if you picked Option A)

Add this to your shell so the rest of the guide reads naturally:

#### Linux / macOS

```sh
# in ~/.bashrc or ~/.zshrc
esphome() {
  docker run --rm -it \
    -v "$PWD":/config \
    --device=/dev/ttyACM0 \
    --device=/dev/ttyUSB0 \
    --network=host \
    ghcr.io/esphome/esphome:stable "$@"
}
```

Then `source ~/.bashrc` (or open a new terminal). The `--device`
lines are harmless if those paths don't exist; ESPHome falls back
to OTA flashing.

#### Windows (PowerShell, OTA-only)

```powershell
# in $PROFILE
function esphome {
  docker run --rm -it `
    -v "${PWD}:/config" `
    --network=host `
    ghcr.io/esphome/esphome:stable @args
}
```

Reload with `. $PROFILE`. Note this Windows wrapper has **no
`--device` line** — Docker Desktop on Windows can't pass through
COM ports cleanly, so this only works for OTA updates after a
first flash. For the initial USB flash, use Option B (pip).

---

## Step 2 — Get the source

```sh
git clone https://github.com/jrnker/Respeaker-XVF3800-VoiceBuddy-integration.git
cd Respeaker-XVF3800-VoiceBuddy-integration
```

The interesting bits for this guide:

```
config/
├── voicebuddy-satellite-minimal.yaml   ← the file you'll edit and flash
└── respeaker-xvf-satellite-example.yaml (formatBCE's full HA example, for reference)
esphome/components/
├── aic3104/                (codec driver — leave alone)
├── respeaker_xvf3800/      (XVF3800 driver — leave alone)
└── voicebuddy_satellite/   (our BARK component — leave alone)
```

You will only edit one file: `config/voicebuddy-satellite-minimal.yaml`.

---

## Step 3 — Set your hub IP and WiFi credentials

### 3a — Edit the YAML

Open `config/voicebuddy-satellite-minimal.yaml` and change the
`substitutions:` block at the top:

```yaml
substitutions:
  hub_host: "192.168.1.50"   # ← put your hub's LAN IP here
  hub_port: "9102"           # leave as-is unless you changed BARK_PORT on the hub
  room_id: "kitchen"         # any short ASCII name; the hub uses this to label commands
```

### 3b — Add your WiFi secrets

ESPHome reads WiFi credentials from a separate file so they don't
end up in git. Create `config/secrets.yaml`:

```yaml
wifi_ssid: "MyHomeWiFi"
wifi_password: "supersecret"
```

(That file is git-ignored already.)

---

## Step 4 — First flash (over USB)

This is the only step that needs the cable. After this, all updates
go over WiFi (OTA), no cable needed.

### 4a — Plug in the ReSpeaker

Plug the ReSpeaker into your computer with the USB-C data cable.
A new serial device should appear:

- **Linux:** `/dev/ttyACM0` or `/dev/ttyUSB0` — check with
  `ls /dev/tty* | grep -E "ACM|USB"`. If your user can't read it,
  add yourself to the `dialout` group:
  `sudo usermod -aG dialout $USER` and log out / back in.
- **macOS:** `/dev/cu.usbmodem*` — find it with `ls /dev/cu.*`.
- **Windows:** open **Device Manager** → expand **Ports (COM &
  LPT)**. You should see a new entry like *"USB Serial Device
  (COM5)"* or *"Silicon Labs CP210x ... (COM5)"*. Note the `COMn`
  number — you'll pick it from a menu in step 4b.

  The XIAO ESP32S3 uses native USB-CDC, which Windows 10/11
  recognise without a driver install. If your board is an older
  revision with a CP210x or CH340 USB-serial chip and Windows
  shows a yellow warning triangle, install the matching driver:
  - CP210x: <https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers>
  - CH340:  <https://www.wch-ic.com/downloads/CH341SER_ZIP.html>

  After plugging the cable in, give Windows ~5 seconds to enumerate
  the device before refreshing Device Manager.

### 4b — Compile and upload

From the project root:

```sh
cd config
esphome run voicebuddy-satellite-minimal.yaml
```

(Same command on Windows PowerShell — `cd` and `esphome run` work
identically there.)

ESPHome will:

1. Download all dependencies (this takes 5–10 minutes the first time;
   subsequent builds are fast).
2. Compile the firmware (~2 minutes).
3. Ask which port to use — pick the one matching your ReSpeaker
   (`/dev/ttyACM0` on Linux, `/dev/cu.usbmodem...` on macOS, `COMn`
   on Windows).
4. Flash the firmware over USB.
5. Start showing live logs from the device.

You'll see WiFi connecting, then a line like:

```
[I][voicebuddy:xxx]: connected to hub 192.168.1.50:9102
[I][voicebuddy:xxx]: session opened id=1
```

That's the BARK handshake — your satellite is talking to the hub.

> **If the build fails** with "framework version" or
> "esp-idf" errors: update ESPHome (`docker pull
> ghcr.io/esphome/esphome:stable` or `pip install -U esphome`) and
> retry. The component requires a recent ESPHome release.

---

## Step 5 — Test it

Say the wake word configured in the YAML (`Hey Jarvis` by default —
change to `okay_nabu` or another supported model in the
`micro_wake_word:` block if you prefer). The satellite's LED ring
will not light up yet (LED frames are v0.2 work — see
`docs/PROTOCOLS.md` §5 in the [VoiceHA](https://github.com/jrnker/VoiceHA)
repo). But on the hub you should see something like:

```
bark.hello peer=('192.168.1.42', 51234) room=kitchen sat_id=...
bark.wake room=kitchen wake_id=1 conf=255
bark.vad end_of_speech room=kitchen bytes=48000
stt room=kitchen text='vad är klockan'
turn room=kitchen total_ms=1240 stt_ms=420 dispatch_ms=80 tts_ms=510
```

…and the satellite plays the spoken reply.

If you don't see those lines, check the troubleshooting section
below.

---

## Step 6 — Subsequent updates (OTA)

After the first USB flash, future builds upload over WiFi:

```sh
cd config
esphome run voicebuddy-satellite-minimal.yaml --device <satellite-ip>
```

Find `<satellite-ip>` in your router's DHCP lease list, or by
copying it from the first-flash log line that says `wifi: assigned
IP ...`.

ESPHome auto-discovers ESPHome devices on the local network too —
running `esphome run ...` without `--device` may give you a menu
including the satellite by name.

---

## Troubleshooting

**`Permission denied` on `/dev/ttyACM0` (Linux):**
You're not in `dialout`. Run `sudo usermod -aG dialout $USER`,
log out, and log back in.

**Compile fails with "external_components: source not found":**
You ran `esphome run` from the wrong directory. The YAML uses
`source: type: local, path: ../esphome/components` — you must run
from `config/`.

**Compile succeeds, satellite boots, but no BARK connection:**
Three things to check, in order.

1. Is the hub actually listening on 9102?
   On the hub host: `ss -tlnp | grep 9102` should show the
   orchestrator process bound to that port.
2. Is your firewall in the way? Try connecting to the BARK port
   from another machine on the same LAN:
   - **Linux / macOS:** `nc -vz <hub-ip> 9102`
   - **Windows (PowerShell):** `Test-NetConnection -ComputerName <hub-ip> -Port 9102`
     (look for `TcpTestSucceeded : True`).
3. Did the satellite get a routable IP? Check the boot logs for
   `wifi: assigned IP ...` — `0.0.0.0` or no log line means the
   WiFi credentials in `secrets.yaml` are wrong.

**Windows: `esphome` not found / "command not recognized":**
You opened a fresh terminal and didn't re-activate the venv.
Run `& $HOME\.venv\esphome\Scripts\Activate.ps1` again. (Or add
the venv's `Scripts` directory to your PATH for the long term.)

**Windows: "Access denied" on the COM port:**
Some other application has the port open — common culprits are the
Arduino IDE Serial Monitor, PuTTY, or another ESPHome instance.
Close them and try again. Unplugging and replugging the USB cable
also resets the OS-side handle.

**Wake word never fires:**
microWakeWord works best with a clear command voice and minimal
background noise the first time you test. The default cutoffs are
moderately strict to avoid false wakes — if it doesn't fire, try
saying it more clearly or change the model. The supported models
are listed in the upstream
[microWakeWord repo](https://github.com/kahrendt/microWakeWord).

**Audio plays back distorted or silent:**
The minimal YAML uses a `resampler` speaker chain that converts the
hub's 16 kHz mono PCM-16 into the 48 kHz stereo the I2S DAC expects.
If you hear static, double-check that the I2S pin numbers in the
YAML match your XVF3800 board revision (look at the silkscreen on
the back) — pin assignments occasionally change between hardware
revisions.

---

## What's next

Once your first satellite is working, the iteration loop is:

1. Edit `voicebuddy-satellite-minimal.yaml` (or graduate to a fuller
   YAML — copy from `respeaker-xvf-satellite-example.yaml` and
   replace its `voice_assistant:` + `api:` blocks with the
   `voicebuddy_satellite:` block from the minimal example).
2. Re-run `esphome run voicebuddy-satellite-minimal.yaml` (no
   cable needed after the first flash).
3. Watch the hub logs to verify behaviour.

For the wire protocol details (frame format, wake events,
TTS playback chunking, future LED frames), see
`docs/PROTOCOLS.md` §5 in the
[VoiceHA](https://github.com/jrnker/VoiceHA) repo.
