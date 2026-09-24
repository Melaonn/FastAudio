# FastAudio

[![Build and test](https://github.com/Melaonn/FastAudio/actions/workflows/build.yml/badge.svg)](https://github.com/Melaonn/FastAudio/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**Low-latency Android game audio to Windows over USB.** FastAudio was built for
BGMI / PUBG Mobile livestreams where screen-mirroring software adds noticeable
audio delay. It captures game playback on the phone, transports raw PCM through
ADB, and renders it on a Windows output device. OBS can capture that same output
in shared mode. Video capture stays in your existing streaming setup.

FastAudio is a **build-from-source CLI project**. The current path has been
developed on an iQOO I2401; audio-policy behavior varies by Android firmware.
Run `probe` before relying on it for a stream.

## What it does

- Captures 48 kHz, stereo, 16-bit game audio through an Android `AudioPolicy`
  loopback mix, with package UID filtering when the phone supports it.
- Sends PCM over a dedicated ADB-forwarded socket, independently of video
  capture and encoding.
- Plays through an event-driven WASAPI renderer with a bounded PCM ring buffer
  and clock-drift correction.
- Qualifies a phone and Windows output device before playback, then caches the
  selected reserve for that combination.
- Offers optional Windows microphone injection into the selected game's Android
  `MIC` / `VOICE_COMMUNICATION` input on a separate transport.

```text
Android game -> AudioPolicy / AudioRecord -> USB / ADB socket
             -> Windows PCM ring -> WASAPI output -> headset / OBS Desktop Audio

Optional: Windows microphone -> separate USB / ADB socket -> Android game mic
```

FastAudio does **not** capture video, encode a stream, add an OBS plugin, or
modify the phone's audio HAL. See [Architecture](docs/ARCHITECTURE.md) and the
[wire protocol](protocol/PROTOCOL.md) for implementation details.

## Requirements

| Component | Requirement |
| --- | --- |
| PC | Windows 11; Visual Studio with the MSVC x64 C++ workload **or** MSYS2 UCRT64 g++ at `C:\msys64` |
| Java | JDK 17, with `javac` and `jar` on `PATH` or under `JAVA_HOME` |
| Android build | SDK platform 36 or newer and build-tools containing `d8` |
| Phone | Android 13 or newer, USB debugging enabled, ADB authorization granted |
| Streaming | OBS or another app that can capture the chosen Windows output device, if you want livestream audio |

The build finds the Android SDK through `ANDROID_HOME`, `ANDROID_SDK_ROOT`, or
`%LOCALAPPDATA%\Android\Sdk`. The engine finds `adb.exe` through `--adb`, the
`ADB` environment variable, `PATH`, or the standard Android SDK platform-tools
directory, in that order. Connect the phone by USB before running FastAudio.

## Quick start: BGMI audio in OBS

```powershell
git clone https://github.com/Melaonn/FastAudio.git
cd FastAudio
.\scripts\build.ps1
adb devices
.\scripts\run.ps1 -Serial YOUR_DEVICE_SERIAL -Command probe
.\scripts\run.ps1 -Serial YOUR_DEVICE_SERIAL -Obs
```

Replace `YOUR_DEVICE_SERIAL` with the value shown by `adb devices`. The launcher
defaults to BGMI's `com.pubg.imobile` package; use `-Package PACKAGE_NAME` for
another game. On the first `run`, FastAudio spends about 12 seconds qualifying
the phone and output endpoint, then starts playback. Stop it with `Ctrl+C`.

In OBS, enable **Desktop Audio** for the same Windows output device FastAudio
uses. `-Obs` selects WASAPI shared mode so both FastAudio and OBS can use it.
Without `-Obs`, the engine uses exclusive mode for direct monitoring.

To send a Windows microphone to the game's team voice chat:

```powershell
.\scripts\run.ps1 -Serial YOUR_DEVICE_SERIAL -Obs -Mic
```

Choose a capture device from the numbered list, or `0` to keep playback only.
The game's own team-mic button still controls when teammates hear you. Mic
injection depends on the phone accepting the privileged Android policy; if it
does not, playback continues and the game's phone microphone remains available.

## Commands and diagnostics

| Command | Purpose |
| --- | --- |
| `probe` | Check package resolution, Android audio-policy registration, UID filtering, voice capture rule, and protocol negotiation. |
| `qualify` | Warm up for 2 seconds, measure for 10 seconds, and select a stable playback reserve. |
| `run` | Load a matching qualification or create one, then play audio. |
| `diagnostics` | Run with one-second queue, correction, and underrun metrics. |

For example:

```powershell
.\scripts\run.ps1 -Serial YOUR_DEVICE_SERIAL -Command diagnostics -Obs
.\scripts\run.ps1 -Serial YOUR_DEVICE_SERIAL -Command qualify -Obs -Requalify
.\scripts\run.ps1 -Serial YOUR_DEVICE_SERIAL -Command diagnostics -Obs -Legacy
```

`-Legacy` uses the preserved 128-frame transport and controller if a device
behaves worse with the burst path. `-LatencyMs 2` is a diagnostic reserve
override; FastAudio raises it if the Windows endpoint requires more. The engine
also exposes options such as `--microphone`, `--shared`, and `--adb` through
`.\windows\build\FastAudio.exe --help`.

Qualification files are stored under
`%LOCALAPPDATA%\FastAudio\qualifications`. They are keyed to the Android build,
package, output endpoint, rendering mode, packet size, and controller revision.
Use `-Requalify` after a phone, driver, or output-device change.

## Latency and compatibility

The reported **estimated floor** combines the Android capture period, the
qualified reserve, and the measured Windows endpoint floor. It excludes OBS
monitoring, recording, and streaming buffers; it is **not** an end-to-end
stream latency measurement. On the tested I2401 firmware, Android provides a
1,024-frame remote-submix period at 48 kHz (21.33 ms). FastAudio avoids adding
another full Android period but cannot remove that device-side floor without a
modified system or vendor audio implementation.

Android hidden APIs and OEM audio rules determine what can be captured. `probe`
reports whether package UID filtering and the voice-capable rule were accepted;
an accepted rule does not guarantee that teammate voice is audible. If UID
filtering is rejected, FastAudio falls back to matching game/media usages and
may capture audio from other apps on the phone. Test this before going live.
Vivo/iQOO firmware receives a temporary teammate-voice routing request that is
removed on shutdown. Other phones need device-specific verification.

In `diagnostics`, a healthy run has no sustained growth in `concealed`,
`hardUnderruns`, `rebuffers`, `dropped`, or `resyncs`. The instantaneous `queue`
value rises and falls by roughly one Android capture period, which is normal.
If qualification rejects a device, the current route did not meet its stability
threshold; the tool does not force playback with an unqualified setting.

## Development

```powershell
.\scripts\build.ps1
.\scripts\test.ps1
```

The automated tests cover Windows qualification logic. The GitHub Actions
workflow builds both components and runs those tests on Windows. Hardware
behavior still needs a real Android phone and Windows audio endpoint.

| Path | Contents |
| --- | --- |
| `android/src/` | Android capture, OEM routing, and optional mic injection daemon |
| `windows/src/` | ADB lifecycle, transport, qualification, WASAPI playback, and microphone capture |
| `protocol/` | Wire format and packet definitions |
| `tests/` | Qualification tests |
| `scripts/` | PowerShell build, run, and test commands |

Bug reports and contributions are welcome through [GitHub Issues](https://github.com/Melaonn/FastAudio/issues)
and pull requests. Include the phone model, Android version, output device, and
redacted `probe` or `diagnostics` output. See [CONTRIBUTING.md](CONTRIBUTING.md).

FastAudio is released under the [MIT License](LICENSE).
