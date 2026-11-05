@echo off
setlocal
cd /d "%~dp0"
set "USER_CFG=%~dp0..\Crome\config"
if not exist "%USER_CFG%" mkdir "%USER_CFG%"
if exist "%USER_CFG%\healing.json" copy /Y "%USER_CFG%\healing.json" "%USER_CFG%\healing_backup.json" >nul
copy /Y "%~dp0config\healing.json" "%USER_CFG%\healing.json" >nul
echo [OK] healing.json atualizado para o modelo v7.2.
pause
