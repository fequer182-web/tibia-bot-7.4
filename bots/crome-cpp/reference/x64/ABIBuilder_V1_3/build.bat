@echo off
setlocal
cd /d "%~dp0"
set "CXX=C:\msys64\ucrt64\bin\g++.exe"
if not exist "%CXX%" (
  echo [ERRO] Compilador nao encontrado: %CXX%
  echo Ajuste CXX no build.bat se seu MSYS2 estiver em outro local.
  pause
  exit /b 1
)
if not exist Build mkdir Build

echo ============================================================
echo    BUILD OTCLab ABI Builder V1.3 X64 AUTOCALIBRATOR CALLER EVIDENCE DLL
echo ============================================================
"%CXX%" -std=c++20 -O2 -Wall -Wextra -shared -static -static-libgcc -static-libstdc++ ^
  Source\OTCLab_ABIBuilder_V1_3_X64_AUTOCALIBRATOR_CALLER_EVIDENCE.cpp ^
  -o Build\OTCLab_ABIBuilder_V1_3_X64_AUTOCALIBRATOR_CALLER_EVIDENCE.dll
if errorlevel 1 (
  echo.
  echo [FAIL] Compilacao falhou.
  pause
  exit /b 1
)

echo.
echo [OK] Build\OTCLab_ABIBuilder_V1_3_X64_AUTOCALIBRATOR_CALLER_EVIDENCE.dll
echo [OK] DLL compilada. Os profiles ficam somente em profiles\ para evitar duplicatas.
echo.
echo IMPORTANTE:
echo - Coloque/substitua em profiles\ o JSON MAIS NOVO da build.
echo - A DLL atualiza o MESMO JSON e cria .pre_abi.bak.json.
echo - IDs nao precisam ser digitados nos testes Look/Move/Use/UseWith.
echo - V1.3 preserva temporarios e grava automaticamente CALL site/caller natural no JSON.
echo - Conclusao: retorno/refcount, vetor autoWalk, getters e chamadas ativas guiadas.
echo - callable depende do adaptador runtime; evidencias completas ficam em abiCompletion.
pause
