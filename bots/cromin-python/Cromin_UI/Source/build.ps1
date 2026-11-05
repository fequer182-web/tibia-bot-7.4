$ErrorActionPreference = 'Stop'
$sourceFolder = $PSScriptRoot
$outputFolder = Split-Path -Parent $sourceFolder
$compiler = 'C:\msys64\ucrt64\bin\g++.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw 'Ajuste o caminho do g++ x64 no build.ps1.' }
$jobs = @(
    @('cromin_ui.cpp', 'Cromin_UI.dll', '-shared', '-lcomctl32', '-lgdi32', '-luser32'),
    @('cromin_ui.cpp', 'Cromin_UI_Preview.exe', '-DCROMIN_UI_PREVIEW', '-lcomctl32', '-lgdi32', '-luser32'),
    @('inject_ui.cpp', 'Cromin_UI_Load.exe', '-lshell32'),
    @('ui_dll_test.cpp', 'Cromin_UI_Dll_Test.exe', '-luser32'),
    @('rat_attack_once.cpp', 'OTC_Rat_Attack_Once.exe'),
    @('corpse_open_once.cpp', 'OTC_Corpse_Open_Once.exe'),
    @('loot_once.cpp', 'OTC_Loot_Once.exe'),
    @('talk_once.cpp', 'OTC_Talk_Once.exe'),
    @('autowalk_once.cpp', 'OTC_AutoWalk_Once.exe')
)
foreach ($job in $jobs) {
    $compileArgs = @('-std=c++17', '-O2', '-Wall', '-Wextra', '-static', '-s', (Join-Path $sourceFolder $job[0]), '-o', (Join-Path $outputFolder $job[1]))
    if ($job.Count -gt 2) { $compileArgs += $job[2..($job.Count - 1)] }
    & $compiler @compileArgs
    if ($LASTEXITCODE -ne 0) { throw "Falha compilando $($job[0])" }
}
