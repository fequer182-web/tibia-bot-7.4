@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build
set CXX=C:\msys64\ucrt64\bin\g++.exe
if not exist "%CXX%" set CXX=g++
echo [1/2] Compilando DLL...
"%CXX%" -std=c++20 -O2 -shared -static-libgcc -static-libstdc++ src\PlayerLightProbe.cpp -o build\CromeClassicPlayerLightProbe.dll -lkernel32 -luser32
if errorlevel 1 goto :fail
echo [2/2] Compilando injector...
"%CXX%" -std=c++20 -O2 -static-libgcc -static-libstdc++ src\Injector.cpp -o build\CromeClassicPlayerLightProbeInjector.exe -lkernel32
if errorlevel 1 goto :fail
echo.
echo [OK] Build concluida em .\build
pause
exit /b 0
:fail
echo.
echo [FAIL] Compilacao falhou.
pause
exit /b 1
