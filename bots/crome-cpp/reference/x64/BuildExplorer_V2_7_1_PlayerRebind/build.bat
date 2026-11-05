@echo off
setlocal
cd /d "%~dp0"

set "CXX=C:\msys64\ucrt64\bin\g++.exe"
if not exist "%CXX%" (
  echo [FAIL] Compilador nao encontrado: %CXX%
  echo Ajuste CXX no build.bat se seu MSYS2 estiver em outro caminho.
  pause
  exit /b 1
)

if not exist Build mkdir Build

rem Keep known reference profiles beside the DLL so the loader can find them.
rem Never overwrite a profile the user already generated in Build.
if not exist "Build\OTCLab_BuildProfile_6AA3019A_602E746BAE706D6C.json" (
  copy /Y "profiles\reference\OTCLab_BuildProfile_6AA3019A_602E746BAE706D6C_X64_FEATURES.json" "Build\OTCLab_BuildProfile_6AA3019A_602E746BAE706D6C.json" >nul
)
if not exist "Build\OTCLab_BuildProfile_6AA9FA57_89D3D35243890EA2.json" (
  copy /Y "profiles\reference\OTCLab_BuildProfile_6AA9FA57_89D3D35243890EA2_X64_FEATURES.json" "Build\OTCLab_BuildProfile_6AA9FA57_89D3D35243890EA2.json" >nul
)

if not exist "Build\OTCLab_BuildProfile_6AA9FB5B_9F444FE4962780AE.json" (
  copy /Y "profiles\reference\OTCLab_BuildProfile_6AA9FB5B_9F444FE4962780AE.json" "Build\OTCLab_BuildProfile_6AA9FB5B_9F444FE4962780AE.json" >nul
)

if not exist "Build\OTCLab_BuildProfile_6A82C258_15F49CEEE51C6AD.json" (
  copy /Y "profiles\reference\OTCLab_BuildProfile_6A82C258_15F49CEEE51C6AD.json" "Build\OTCLab_BuildProfile_6A82C258_15F49CEEE51C6AD.json" >nul
)

echo ============================================================
echo   OTCLab BuildExplorer V2.7.1 - PLAYER REBIND + LOOK AUTO + LIGHT
echo ============================================================
echo [1/1] Compilando DLL x64...

"%CXX%" -std=c++20 -O2 -Wall -Wextra -shared -static-libgcc -static-libstdc++ ^
  "Source\OTCLab_BuildExplorer_V2_7_1_X64_PLAYER_REBIND.cpp" ^
  -o "Build\OTCLab_BuildExplorer_V2_7_1_X64_PLAYER_REBIND.dll"

if errorlevel 1 (
  echo.
  echo [FAIL] Compilacao falhou.
  pause
  exit /b 1
)

echo.
echo [OK] DLL gerada:
echo %CD%\Build\OTCLab_BuildExplorer_V2_7_1_X64_PLAYER_REBIND.dll
echo.
echo IMPORTANTE:
echo - O discovery principal continua READ-ONLY.
echo - Look novo usa auto-discovery por breakpoint temporario; Light usa calibracao + write/restore opcional.
echo - Use somente esta DLL durante o teste; feche probes/hooks antigos antes de injetar.
echo.
echo Injete esta DLL com o mesmo injector x64 usado no BuildExplorer.
pause
