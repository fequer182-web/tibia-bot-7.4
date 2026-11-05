@echo off
setlocal
cd /d "%~dp0"
if not exist build\CromeClassicPlayerLightProbe.dll (
  echo [FAIL] DLL nao encontrada. Rode build.bat primeiro.
  pause
  exit /b 1
)
build\CromeClassicPlayerLightProbeInjector.exe build\CromeClassicPlayerLightProbe.dll
pause
