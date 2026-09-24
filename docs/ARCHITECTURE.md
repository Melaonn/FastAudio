# Architecture

## Data path

```text
selected Android package UID
  -> AudioPolicy / AudioMix loopback
  -> AudioRecord (48 kHz, stereo, S16, one HAL quantum; 1024 on I2401)
  -> localabstract:fastaudio
  -> adb forward to localhost
  -> packet receiver thread
  -> preallocated SPSC PCM ring
  -> MMCSS Pro Audio WASAPI render thread
  -> selected Windows headset

Optional PC microphone path:

Windows capture endpoint -> shared WASAPI capture -> bounded 48 kHz mono queue
  -> dedicated ADB-forwarded socket -> Android AudioPolicy injector
  -> selected game's `MIC` / `VOICE_COMMUNICATION` recorder (PUBG team voice)
```

Video software is outside this process. A stalled video encoder, decoder,
renderer, or capture window cannot hold the audio socket or render callback.

## Threading

Android uses one blocking capture loop. Its buffers and protocol packet are
allocated before recording begins.

Windows uses:

- an ADB lifecycle process;
- a network receiver thread that is the only ring producer;
- a WASAPI event thread that is the only ring consumer.
- when enabled, an independent shared-WASAPI capture thread and mic sender.

The render path performs no logging, process management, socket operations, or
dynamic allocation. Metrics are atomics read by the CLI thread.

The microphone path is deliberately independent. It has its own socket and a
60 ms bounded Windows queue, so a missing microphone or a blocked USB transfer
cannot add latency to, or corrupt, game playback. Closing that socket removes
the injector policy and restores the phone microphone for the game.

## Latency policy

Qualification is intentionally separate from runtime drift correction.

- Qualification selects and locks the post-period reserve.
- Startup is phase-aligned by the reserve plus the Windows endpoint period.
- Runtime control targets the midpoint of the Android burst sawtooth and may
  resample within +/-0.1% to correct clock drift.
- One Android capture quantum is retained as useful audio; older excess is
  trimmed instead of becoming permanent latency.
- A short shortage is concealed; a longer shortage is counted as a hard
  underrun and faded to silence.
- Runtime events never permanently increase the locked reserve.

`--legacy` restores 128-frame packets, the original queue controller and its
shared-WASAPI initialization. Existing legacy qualification caches remain
valid.

## Certification

The qualification key contains:

- Android build fingerprint;
- selected package;
- Windows endpoint ID;
- endpoint mode, initialization path, period, buffer size, and latency floor;
- packet size and controller revision;
- protocol version.
- qualifier revision.

The low-latency estimate includes the Android packet quantum, locked reserve,
and Windows endpoint floor. Results:

- Competitive: estimated floor <= 24 ms
- Standard: estimated floor <= 35 ms
- Unsupported: no acceptable reserve through 40 ms
