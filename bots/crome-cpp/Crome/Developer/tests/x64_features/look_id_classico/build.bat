@echo off
setlocal
cd /d "%~dp0"
set CXX=C:\msys64\ucrt64\bin\g++.exe
if not exist "%CXX%" (
  echo [FAIL] Compiler nao encontrado: %CXX%
  pause
  exit /b 1
)
echo [1/2] Compilando DLL...
"%CXX%" -std=c++20 -O2 -shared src\FeatureProbe.cpp -o CromeClassicFeatureProbe.dll -static-libgcc -static-libstdc++
if errorlevel 1 goto :fail
echo [2/2] Compilando injector...
"%CXX%" -std=c++20 -O2 src\Injector.cpp -o CromeClassicProbeInjector.exe -static-libgcc -static-libstdc++
if errorlevel 1 goto :fail
echo.
echo [OK] Build concluido.
echo DLL: CromeClassicFeatureProbe.dll
echo EXE: CromeClassicProbeInjector.exe
pause
exit /b 0
:fail
echo.
echo [FAIL] Compilacao falhou.
pause
exit /b 1
