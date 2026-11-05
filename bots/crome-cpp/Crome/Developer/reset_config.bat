@echo off
setlocal
cd /d "%~dp0"
set "USER_DIR=%~dp0..\Crome\config"
if not exist "%USER_DIR%" mkdir "%USER_DIR%"
copy /Y "%~dp0config\*.json" "%USER_DIR%\" >nul
echo [OK] Configuracoes padrao v7.1 restauradas em:
echo %USER_DIR%
pause
