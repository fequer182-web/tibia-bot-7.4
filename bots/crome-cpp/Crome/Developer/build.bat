@echo off
setlocal
cd /d "%~dp0"

set "USER_DIR=%~dp0..\Crome"
set "BUILD_DIR=%LOCALAPPDATA%\CromeBuild\Developer"
set "CROME_SRC_DRIVE="

echo ============================================================
echo                 Crome - BUILD RELEASE
echo ============================================================
echo.

rem MinGW Makefiles/FetchContent can corrupt UTF-8 paths such as "Area de Trabalho"
rem when the repository lives under an accented Windows directory. Mount the
rem Developer folder on a temporary ASCII-only drive and keep the CMake build
rem tree under LOCALAPPDATA, which also avoids stale absolute paths from old builds.
for %%D in (V W X Y Z) do (
    if not exist "%%D:\" if not defined CROME_SRC_DRIVE set "CROME_SRC_DRIVE=%%D:"
)

if not defined CROME_SRC_DRIVE (
    echo [ERRO] Nenhuma letra livre entre V: e Z: para o build ASCII.
    goto :error
)

for %%I in ("%~dp0.") do set "REAL_DEV_DIR=%%~fI"
subst %CROME_SRC_DRIVE% "%REAL_DEV_DIR%"
if errorlevel 1 (
    echo [ERRO] Nao foi possivel criar o drive temporario %CROME_SRC_DRIVE%
    goto :error
)

set "SRC_DIR=%CROME_SRC_DRIVE%/"

echo [PATH] Source real : %REAL_DEV_DIR%
echo [PATH] Source CMake: %SRC_DIR%
echo [PATH] Build       : %BUILD_DIR%
echo.

if exist "%BUILD_DIR%\CMakeCache.txt" (
    findstr /I /C:"CMAKE_HOME_DIRECTORY:INTERNAL=%SRC_DIR%" "%BUILD_DIR%\CMakeCache.txt" >nul 2>&1
    if errorlevel 1 (
        echo [INFO] Cache CMake antigo detectado. Limpando somente o build externo...
        rmdir /S /Q "%BUILD_DIR%"
    )
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -S %SRC_DIR% -B "%BUILD_DIR%" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCROME_BUILD_DEV_TOOLS=OFF
if errorlevel 1 goto :error

cmake --build "%BUILD_DIR%" --target Crome CromeRuntime --config Release -j 1
if errorlevel 1 goto :error

if not exist "%BUILD_DIR%\bin\Crome.exe" (
    echo [ERRO] O linker terminou sem gerar Crome.exe
    goto :error
)
if not exist "%BUILD_DIR%\bin\CromeRuntime.dll" (
    echo [ERRO] O linker terminou sem gerar CromeRuntime.dll
    goto :error
)

if not exist "%USER_DIR%" mkdir "%USER_DIR%"
if not exist "%USER_DIR%\profiles" mkdir "%USER_DIR%\profiles"
if not exist "%USER_DIR%\config" mkdir "%USER_DIR%\config"

copy /Y "%BUILD_DIR%\bin\Crome.exe" "%USER_DIR%\Crome.exe" >nul
copy /Y "%BUILD_DIR%\bin\CromeRuntime.dll" "%USER_DIR%\CromeRuntime.dll" >nul
xcopy /E /I /Y "%SRC_DIR%profiles\*" "%USER_DIR%\profiles\" >nul
copy /Y "%SRC_DIR%native_runtime\profiles\builds.json" "%USER_DIR%\profiles\builds.json" >nul

if /I "%~1"=="tests" (
    echo.
    echo [TEST] Compilando calibradores x64 Look ID + HackLight...
    call "%SRC_DIR%build_x64_feature_tests.bat"
    if errorlevel 1 goto :error
)

rem Runtime configs: backup current user values, then install a clean OFF-by-default schema.
for %%N in (bot healing targeting loot fishing tools waypoints action_test tile_action_test say_test items inspect_request inspect_result look_result) do (
    if exist "%USER_DIR%\config\%%N.json" copy /Y "%USER_DIR%\config\%%N.json" "%USER_DIR%\config\%%N.backup.json" >nul
    if exist "%SRC_DIR%config\%%N.json" copy /Y "%SRC_DIR%config\%%N.json" "%USER_DIR%\config\%%N.json" >nul
)

rem O catalogo global de IDs e conhecimento do usuario: nunca sobrescrever em rebuild.
if not exist "%USER_DIR%\config\walker_ids.json" (
    copy /Y "%SRC_DIR%config\walker_ids.json" "%USER_DIR%\config\walker_ids.json" >nul
)

echo.
echo ============================================================
echo [OK] Crome gerado com sucesso.
echo [DEV]  %BUILD_DIR%
echo [USER] %USER_DIR%\Crome.exe
echo ============================================================
echo.
set "EXIT_CODE=0"
goto :cleanup

:error
echo.
echo ============================================================
echo [ERRO] A compilacao falhou. Veja o erro acima.
echo ============================================================
echo.
set "EXIT_CODE=1"

:cleanup
if defined CROME_SRC_DRIVE subst %CROME_SRC_DRIVE% /D >nul 2>&1
pause
exit /b %EXIT_CODE%
