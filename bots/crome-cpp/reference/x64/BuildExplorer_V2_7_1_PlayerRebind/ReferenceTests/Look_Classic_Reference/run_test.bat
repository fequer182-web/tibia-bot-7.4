@echo off
cd /d "%~dp0"
if not exist CromeClassicFeatureProbe.dll (
  echo [INFO] DLL ainda nao existe. Rode build.bat primeiro.
  pause
  exit /b 1
)
if not exist CromeClassicProbeInjector.exe (
  echo [INFO] Injector ainda nao existe. Rode build.bat primeiro.
  pause
  exit /b 1
)
CromeClassicProbeInjector.exe
