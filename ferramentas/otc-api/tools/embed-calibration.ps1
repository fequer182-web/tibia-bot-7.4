param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9-]+$')]
    [string]$BuildId,
    [ValidateSet('Win32', 'x64')]
    [string]$Architecture = 'Win32'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$buildName = if ($Architecture -eq 'x64') { 'build-x64' } else { 'build' }
$buildDir = Join-Path $repo $buildName
$releaseDir = Join-Path $repo 'dist'
$runtimeProfile = Join-Path $releaseDir "calibrations\$BuildId.json"
$catalogDir = Join-Path $repo 'configs\calibrations'
$catalogProfile = Join-Path $catalogDir "$BuildId.json"
$dllName = if ($Architecture -eq 'x64') { 'OTCApi-x64.dll' } else { 'OTCApi-Win32.dll' }
$dllPath = Join-Path $releaseDir $dllName

if (-not (Test-Path -LiteralPath $runtimeProfile -PathType Leaf)) { throw "Runtime calibration not found: $runtimeProfile" }
$profile = Get-Content -LiteralPath $runtimeProfile -Raw | ConvertFrom-Json
if ($profile.buildId -ne $BuildId) { throw "Profile buildId '$($profile.buildId)' does not match '$BuildId'" }

New-Item -ItemType Directory -Path $catalogDir -Force | Out-Null
Copy-Item -LiteralPath $runtimeProfile -Destination $catalogProfile -Force
$generatedCatalog = Join-Path $buildDir 'generated\embedded_calibrations.cpp'
if (Test-Path -LiteralPath $generatedCatalog -PathType Leaf) {
    Remove-Item -LiteralPath $generatedCatalog -Force
}
& cmake --build $buildDir --config Release --target OTCApi
if ($LASTEXITCODE -ne 0) { throw "OTCApi build failed with exit code $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath $dllPath -PathType Leaf)) { throw "Built DLL not found: $dllPath" }
$dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dllPath))
if (-not $dllText.Contains($BuildId)) { throw "BuildId was not found in the generated DLL: $BuildId" }

Write-Host "Embedded calibration: $catalogProfile"
Write-Host "Standalone DLL: $dllPath"

