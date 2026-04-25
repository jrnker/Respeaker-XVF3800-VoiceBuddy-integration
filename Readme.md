# Respeaker XVF3800 — VoiceBuddy fork

ESPHome components and example config for the [Respeaker XVF3800](https://github.com/respeaker/reSpeaker_XVF3800_USB_4MIC_ARRAY).

This is the **VoiceBuddy** fork of [`formatBCE/Respeaker-XVF3800-ESPHome-integration`](https://github.com/formatBCE/Respeaker-XVF3800-ESPHome-integration). It keeps the upstream XVF3800 driver, microphone/speaker pipeline, microWakeWord wiring and LED state machine, and replaces the Home Assistant `voice_assistant:` + `api:` blocks with a custom ESPHome component that speaks **BARK** (Binary Audio Realtime Kit) directly to the VoiceBuddy hub. See `docs/PROTOCOLS.md` §5 in the [VoiceHA](https://github.com/jrnker/VoiceHA) repo for the wire spec.

Status: under active development.

**New here?** Start with [`docs/HOWTO-build-and-flash.md`](docs/HOWTO-build-and-flash.md) — step-by-step from a boxed ReSpeaker to a working satellite using ESPHome.

## Upstream credit

All hardware-facing work (XVF3800 driver, I2S plumbing, LED control, wake-word integration) is by [@formatBCE](https://github.com/formatBCE). This fork only swaps the transport layer.

## Known issues (inherited from upstream)

1. No buttons — voice is the only way to stop a timer or response (say "stop"); no manual pipeline trigger.
2. No hardware volume control beyond the software mixer.
3. LED ring is I2C-driven, not exposed as an ESPHome `light`.
