# Contributing to FastAudio

Thanks for helping improve FastAudio. Please open an issue before a large change
so we can agree on the device, Android version, and expected behavior.

## Build and test

On Windows 11, install the tools listed in [README.md](README.md#requirements).
Then run:

```powershell
.\scripts\build.ps1
.\scripts\test.ps1
```

The automated tests cover the Windows qualification logic. A full device test
also needs an Android 13+ phone with USB debugging enabled:

```powershell
adb devices
.\scripts\run.ps1 -Serial DEVICE -Command probe
```

When reporting a device issue, include the phone model, Android version,
Windows output device, the command used, and the `probe` or `diagnostics`
output. Remove device serials and other personal details before posting logs.
OEM audio policy and microphone injection behavior can differ between phones.

## Pull requests

Keep changes focused and explain the affected audio path. Include test results
and, for latency or routing changes, what you heard or measured on a device.
Please avoid committing build outputs, qualification caches, device logs, or
third-party binaries.

By submitting a contribution, you agree that it is licensed under the
[MIT License](LICENSE).
