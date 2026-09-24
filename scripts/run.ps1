param(
    [Parameter(Mandatory = $true)][string]$Serial,
    [string]$Package = "com.pubg.imobile",
    [ValidateSet("probe", "qualify", "run", "diagnostics")]
    [string]$Command = "run",
    [switch]$Obs,
    [switch]$Mic,
    [switch]$Legacy,
    [switch]$Requalify,
    [ValidateRange(2, 40)][double]$LatencyMs
)

$Root = Split-Path -Parent $PSScriptRoot
$Exe = Join-Path $Root "windows\build\FastAudio.exe"
if (-not (Test-Path $Exe)) {
    & (Join-Path $PSScriptRoot "build.ps1")
}
$Arguments = @($Command, "--serial", $Serial, "--package", $Package)
if ($Obs) {
    $Arguments += "--shared"
}
if ($Mic) {
    $Arguments += @("--microphone", "select")
}
if ($Legacy) {
    $Arguments += "--legacy"
}
if ($Requalify) {
    $Arguments += "--requalify"
}
if ($PSBoundParameters.ContainsKey("LatencyMs")) {
    $Arguments += @("--latency-ms", $LatencyMs.ToString(
            [Globalization.CultureInfo]::InvariantCulture))
}
& $Exe @Arguments
