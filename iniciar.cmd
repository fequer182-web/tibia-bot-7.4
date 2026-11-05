@echo off
cd /d "%~dp0"
python iniciar.py %*
if errorlevel 1 pause
