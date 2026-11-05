@echo off
cd /d "%~dp0"
if not exist "..\Crome\Crome.exe" (
  echo [ERRO] ..\Crome\Crome.exe nao existe.
  echo Execute build.bat primeiro.
  pause
  exit /b 1
)
start "" "..\Crome\Crome.exe"
