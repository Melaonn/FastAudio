$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

& (Join-Path $PSScriptRoot "build-android.ps1")
& (Join-Path $PSScriptRoot "build-windows.ps1")

$Daemon = Join-Path $Root "android\build\fastaudio-daemon.jar"
$RuntimeDaemon = Join-Path $Root "windows\build\fastaudio-daemon.jar"
Copy-Item $Daemon $RuntimeDaemon -Force

Write-Host "FastAudio build complete."
