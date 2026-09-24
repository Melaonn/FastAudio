$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root "windows\build"
New-Item $Build -ItemType Directory -Force | Out-Null

$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$Vs = if (Test-Path $VsWhere) {
    & $VsWhere -latest -products * -property installationPath
}

$Main = Join-Path $Root "windows\src\main.cpp"
$Test = Join-Path $Root "tests\qualification_tests.cpp"
$Include = Join-Path $Root "windows\src"
$Exe = Join-Path $Build "FastAudio.exe"
$TestExe = Join-Path $Build "FastAudioTests.exe"

$Built = $false
if ($Vs) {
    $VcVars = Join-Path $Vs "VC\Auxiliary\Build\vcvars64.bat"
    $VcVarsAll = Join-Path $Vs "VC\Auxiliary\Build\vcvarsall.bat"
    if ((Test-Path $VcVars) -and (Test-Path $VcVarsAll)) {
        $EnvironmentLines = & cmd.exe /d /c "call `"$VcVars`" >nul && set"
        if (-not $LASTEXITCODE) {
            foreach ($Line in $EnvironmentLines) {
                $Separator = $Line.IndexOf("=")
                if ($Separator -gt 0) {
                    $Name = $Line.Substring(0, $Separator)
                    $Value = $Line.Substring($Separator + 1)
                    Set-Item -Path "Env:$Name" -Value $Value
                }
            }
            & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /DNDEBUG "/I$Include" `
                $Main "/Fe:$Exe" /link avrt.lib ole32.lib ws2_32.lib
            if ($LASTEXITCODE) { throw "FastAudio Windows build failed" }

            & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 "/I$Include" `
                $Test "/Fe:$TestExe"
            if ($LASTEXITCODE) { throw "FastAudio test build failed" }
            $Built = $true
        }
    }
}

if (-not $Built) {
    $Gpp = "C:\msys64\ucrt64\bin\g++.exe"
    $Bash = "C:\msys64\usr\bin\bash.exe"
    if (-not (Test-Path $Gpp) -or -not (Test-Path $Bash)) {
        throw "Neither a complete MSVC workload nor MSYS2 UCRT64 g++ was found"
    }
    $MsysRoot = $Root.Replace("\", "/")
    if ($MsysRoot -match "^([A-Za-z]):/(.*)$") {
        $MsysRoot = "/" + $Matches[1].ToLowerInvariant() + "/" + $Matches[2]
    }
    $Command = @"
set -euo pipefail
export PATH=/ucrt64/bin:`$PATH
cd '$MsysRoot'
g++ -std=c++20 -O2 -DNDEBUG -Wall -Wextra -static -static-libgcc -static-libstdc++ -Iwindows/src windows/src/main.cpp -o windows/build/FastAudio.exe -lavrt -lole32 -luuid -lws2_32
g++ -std=c++20 -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ -Iwindows/src tests/qualification_tests.cpp -o windows/build/FastAudioTests.exe
"@
    & $Bash -lc $Command
    if ($LASTEXITCODE) { throw "FastAudio MSYS2 build failed" }
}

Write-Host "Windows engine: $Exe"
