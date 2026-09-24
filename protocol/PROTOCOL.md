# FastAudio Wire Protocol v1

All integer fields use network byte order. Audio is interleaved signed
little-endian PCM.

## Hello (32 bytes)

| Offset | Type | Meaning |
|---:|---|---|
| 0 | u32 | `FAUD` magic |
| 4 | u16 | protocol version |
| 6 | u16 | hello size |
| 8 | u32 | sample rate |
| 12 | u16 | channels |
| 14 | u16 | bits per sample |
| 16 | u32 | nominal packet frames (1,024 low-latency; 128 legacy) |
| 20 | u32 | selected package UID |
| 24 | u32 | capability flags |
| 28 | u32 | reserved |

Capability bit 0 indicates that the voice-communication capture rule was
accepted. Bit 1 indicates that the OEM accepted package UID filtering. If bit
1 is clear, the daemon is capturing matching game/media usages system-wide.

## PCM packet

Each packet starts with a 32-byte header:

| Offset | Type | Meaning |
|---:|---|---|
| 0 | u32 | `FAP1` magic |
| 4 | u16 | protocol version |
| 6 | u16 | header size |
| 8 | u64 | sequence number |
| 16 | u64 | capture-end timestamp, Android monotonic nanoseconds |
| 24 | u32 | PCM frame count |
| 28 | u32 | flags |

`frameCount` may be smaller than the nominal value when Android returns a
partial blocking read. The payload immediately follows and contains
`frameCount * channels * bitsPerSample / 8` bytes.

Packet flag bit 0 means the timestamp fell back to `System.nanoTime()`.
Packet flag bit 1 indicates a discontinuity.

# Microphone transport

Playback keeps its original `localabstract:fastaudio` protocol. When a Windows
microphone is selected, FastAudio also forwards `localabstract:fastaudio-mic`
to TCP port 27184. Android first sends a 32-byte `FAMH` hello:

- 48 kHz, mono, 16-bit PCM
- 480 frames per packet (10 ms)
- status bit 1 means the game-specific Android injector was accepted

Windows then sends `FAMP` packets with the same 32-byte header layout as
playback: sequence at byte 8, monotonic timestamp at byte 16, frame count at
byte 24, and discontinuity flags at byte 28. Payload is signed 16-bit mono
PCM. Closing the PC-to-phone socket removes the Android microphone injector.
