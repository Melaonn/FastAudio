param(
    [string]$AndroidSdk
)

$ErrorActionPreference = "Stop"
if (-not $AndroidSdk) {
    $AndroidSdk = @(
        $env:ANDROID_HOME
        $env:ANDROID_SDK_ROOT
        (Join-Path $env:LOCALAPPDATA "Android\Sdk")
    ) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}
if (-not $AndroidSdk) {
    throw "Android SDK not found. Set ANDROID_HOME or pass -AndroidSdk."
}
$Root = Split-Path -Parent $PSScriptRoot
$Android = Join-Path $Root "android"
$Build = Join-Path $Android "build"
$Classes = Join-Path $Build "classes"
$Dex = Join-Path $Build "dex"

$Platform = Get-ChildItem (Join-Path $AndroidSdk "platforms") -Directory |
    Where-Object {
        $_.Name -match '^android-(\d+)(?:\.\d+)?$' -and
        [int]$Matches[1] -ge 36 -and
        (Test-Path (Join-Path $_.FullName "android.jar"))
    } |
    Sort-Object Name -Descending |
    Select-Object -First 1
$BuildTools = Get-ChildItem (Join-Path $AndroidSdk "build-tools") -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName "d8.bat") } |
    Sort-Object Name -Descending |
    Select-Object -First 1

if (-not $Platform -or -not $BuildTools) {
    throw "Android platform 36+ or d8 was not found under $AndroidSdk"
}

$JavacCommand = Get-Command javac.exe -ErrorAction SilentlyContinue
$Javac = if ($JavacCommand) { $JavacCommand.Source }
if (-not $Javac -and $env:JAVA_HOME) {
    $Candidate = Join-Path $env:JAVA_HOME "bin\javac.exe"
    if (Test-Path $Candidate) { $Javac = $Candidate }
}
if (-not $Javac) { throw "javac.exe not found. Install JDK 17 and set JAVA_HOME or PATH." }
$JarCommand = Get-Command jar.exe -ErrorAction SilentlyContinue
$Jar = if ($JarCommand) { $JarCommand.Source }
if (-not $Jar -and $env:JAVA_HOME) {
    $Candidate = Join-Path $env:JAVA_HOME "bin\jar.exe"
    if (Test-Path $Candidate) { $Jar = $Candidate }
}
if (-not $Jar) {
    $Jar = Get-ChildItem "C:\Program Files\Java" -Recurse -Filter jar.exe `
        -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Jar) {
    $Jar = Get-ChildItem "C:\Program Files\Android\Android Studio" -Recurse `
        -Filter jar.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Jar) { throw "jar.exe was not found" }

Remove-Item $Build -Recurse -Force -ErrorAction SilentlyContinue
New-Item $Classes, $Dex -ItemType Directory -Force | Out-Null

$Sources = Get-ChildItem (Join-Path $Android "src") -Recurse -Filter *.java |
    ForEach-Object FullName
& $Javac -source 17 -target 17 -encoding UTF-8 `
    -classpath (Join-Path $Platform.FullName "android.jar") `
    -d $Classes $Sources
if ($LASTEXITCODE) { throw "javac failed" }

$ClassFiles = Get-ChildItem $Classes -Recurse -Filter *.class |
    ForEach-Object FullName
& (Join-Path $BuildTools.FullName "d8.bat") `
    --min-api 33 --output $Dex $ClassFiles
if ($LASTEXITCODE) { throw "d8 failed" }

$DaemonJar = Join-Path $Build "fastaudio-daemon.jar"
& $Jar --create --file $DaemonJar -C $Dex classes.dex
if ($LASTEXITCODE) { throw "jar failed" }

Write-Host "Android daemon: $DaemonJar"
