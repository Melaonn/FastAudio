$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Test = Join-Path $Root "windows\build\FastAudioTests.exe"
$TestSources = @(
    Get-ChildItem (Join-Path $Root "tests") -Recurse -File
    Get-ChildItem (Join-Path $Root "windows\src") -Recurse -File
)
$NeedsBuild = -not (Test-Path $Test)
if (-not $NeedsBuild) {
    $BuiltAt = (Get-Item $Test).LastWriteTimeUtc
    $NeedsBuild = $null -ne ($TestSources |
        Where-Object LastWriteTimeUtc -gt $BuiltAt |
        Select-Object -First 1)
}
if ($NeedsBuild) {
    & (Join-Path $PSScriptRoot "build-windows.ps1")
    if ($LASTEXITCODE) { throw "FastAudio test build failed" }
}
& $Test
if ($LASTEXITCODE) { throw "FastAudio tests failed" }
