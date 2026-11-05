$ErrorActionPreference = 'Stop'
$onceCompiler = 'C:\msys64\ucrt64\bin\g++.exe'
Push-Location -LiteralPath $PSScriptRoot
try {
    & $onceCompiler -std=c++17 -O2 -Wall -Wextra -static -s 'Source\walk_once.cpp' -o 'OTC_Walk_Once.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Falha na compilacao.' }
} finally { Pop-Location }
