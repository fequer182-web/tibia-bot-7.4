@echo off
setlocal
cd /d "%~dp0"

echo ============================================================
echo       Crome x64 feature calibration tests - BUILD
echo ============================================================
echo.

call "%~dp0tests\x64_features\look_id_classico\build.bat"
if errorlevel 1 goto :error
call "%~dp0tests\x64_features\player_light_classico\build.bat"
if errorlevel 1 goto :error

python "%~dp0tests\profile_smoke.py"
if errorlevel 1 goto :error
python "%~dp0tests\feature_profile_smoke.py"
if errorlevel 1 goto :error

echo.
echo [OK] Look ID + Player Light calibration tests compilados.
exit /b 0

:error
echo.
echo [ERRO] Falha ao compilar/validar os testes x64.
exit /b 1
