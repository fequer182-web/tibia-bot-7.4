@echo off
cd /d "%~dp0"
if exist build rmdir /s /q build
echo Build de desenvolvimento removido.
echo A pasta ..\Crome foi preservada.
pause
