@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

set "SRC=%~dp0src\RamoniaResearchProbe.cpp"
set "BUILD_DIR=%~dp0build"
set "PKG_DIR=%BUILD_DIR%\Ramonia_x86_Test"
set "PROFILE_SRC=%~dp0profiles\ramonia_x86\6AB03F94_E281E9CA5BA0AF2.json"
set "PROFILE_DIR=%PKG_DIR%\profiles\ramonia_x86"
set "INJECTOR_CPP=%~dp0..\..\tools\injector\PID_Injector.cpp"
set "OUT_DLL=%PKG_DIR%\RamoniaResearchProbe_V40.dll"
set "OUT_INJECTOR=%PKG_DIR%\PID_Injector_x86.exe"
set "BRIDGE_SRC=%~dp0bridge\CromeRamoniaBridge.cpp"
set "PIPE_TEST_SRC=%~dp0bridge\CromePipeTest.cpp"
set "MINIJSON_DIR=%~dp0..\..\Crome\Developer\native_runtime\profiles"
set "OUT_BRIDGE=%PKG_DIR%\CromeRamoniaBridge_x86.dll"
set "OUT_PIPE_TEST=%PKG_DIR%\CromePipeTest_x64.exe"

echo ============================================================
echo          Crome - Ramonia x86 Research Probe V40
echo                     BUILD X86
echo ============================================================
echo.

if not exist "%SRC%" (
    echo [ERRO] Source nao encontrado:
    echo        %SRC%
    goto :fail
)

set "CXX="

rem Prefer the normal MSYS2 MinGW32 compiler when installed.
if exist "C:\msys64\mingw32\bin\g++.exe" (
    set "CXX=C:\msys64\mingw32\bin\g++.exe"
)

rem Otherwise accept any i686 MinGW compiler available in PATH.
if not defined CXX (
    for /f "delims=" %%G in ('where i686-w64-mingw32-g++.exe 2^>nul') do (
        if not defined CXX set "CXX=%%G"
    )
)

rem Last fallback: inspect g++.exe from PATH, but architecture is checked below.
if not defined CXX (
    for /f "delims=" %%G in ('where g++.exe 2^>nul') do (
        if not defined CXX set "CXX=%%G"
    )
)

if not defined CXX (
    echo [ERRO] Nenhum compilador g++ encontrado.
    echo.
    echo Este probe precisa de um compilador MinGW x86 ^(i686^).
    echo Exemplo esperado:
    echo   C:\msys64\mingw32\bin\g++.exe
    goto :fail
)

for %%I in ("%CXX%") do set "CXX_DIR=%%~dpI"
set "PATH=%CXX_DIR%;%PATH%"

for /f "delims=" %%T in ('"%CXX%" -dumpmachine 2^>nul') do set "TARGET=%%T"

echo [COMPILER] %CXX%
echo [CXX BIN]  %CXX_DIR%
echo [TARGET]   %TARGET%
echo.

echo %TARGET% | findstr /I /C:"i686" /C:"i386" >nul
if errorlevel 1 (
    echo [ERRO] O compilador encontrado NAO e x86.
    echo.
    echo Target detectado: %TARGET%
    echo O Ramonia Research Probe possui static_assert de 32 bits e deve ser
    echo compilado com i686/MinGW32. Nao use UCRT64/x86_64 para esta DLL.
    goto :fail
)

if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
mkdir "%PROFILE_DIR%" >nul 2>&1

echo [BUILD] Compilando RamoniaResearchProbe_V40.dll...
"%CXX%" ^
    -std=c++20 ^
    -O2 ^
    -shared ^
    -static ^
    -static-libgcc ^
    -static-libstdc++ ^
    "%SRC%" ^
    -o "%OUT_DLL%"

if errorlevel 1 (
    echo.
    echo [ERRO] A compilacao da DLL falhou.
    goto :fail
)

if not exist "%OUT_DLL%" (
    echo [ERRO] O linker terminou sem gerar:
    echo        %OUT_DLL%
    goto :fail
)

echo.
echo [DEPENDENCIAS] DLLs importadas:
if exist "%CXX_DIR%objdump.exe" (
    "%CXX_DIR%objdump.exe" -p "%OUT_DLL%" | findstr /I /C:"DLL Name"
    "%CXX_DIR%objdump.exe" -p "%OUT_DLL%" | findstr /I /C:"libwinpthread-1.dll" >nul
    if not errorlevel 1 (
        echo.
        echo [ERRO] A DLL ainda depende de libwinpthread-1.dll.
        echo        O build deve ser autocontido antes do teste.
        goto :fail
    )
) else (
    echo [AVISO] objdump.exe nao encontrado; auditoria automatica pulada.
)

copy /Y "%PROFILE_SRC%" "%PROFILE_DIR%\6AB03F94_E281E9CA5BA0AF2.json" >nul
if errorlevel 1 (
    echo [ERRO] Falha ao copiar o profile canonico.
    goto :fail
)

if not exist "%BRIDGE_SRC%" (
    echo [ERRO] Source da bridge nao encontrado:
    echo        %BRIDGE_SRC%
    goto :fail
)

echo.
echo [BUILD] Compilando CromeRamoniaBridge_x86.dll...
"%CXX%" ^
    -std=c++20 ^
    -O2 ^
    -shared ^
    -static ^
    -static-libgcc ^
    -static-libstdc++ ^
    -I"%MINIJSON_DIR%" ^
    "%BRIDGE_SRC%" ^
    -o "%OUT_BRIDGE%"

if errorlevel 1 (
    echo.
    echo [ERRO] A compilacao da bridge x86 falhou.
    goto :fail
)

if not exist "%OUT_BRIDGE%" (
    echo [ERRO] O linker terminou sem gerar:
    echo        %OUT_BRIDGE%
    goto :fail
)

echo.
echo [AUDITORIA] Bridge x86...
"%CXX_DIR%objdump.exe" -f "%OUT_BRIDGE%" | findstr /I /C:"pei-i386" >nul
if errorlevel 1 (
    echo [ERRO] CromeRamoniaBridge_x86.dll nao e PE32/i386.
    goto :fail
)
"%CXX_DIR%objdump.exe" -p "%OUT_BRIDGE%" | findstr /I /C:"libwinpthread-1.dll" /C:"libgcc_s_dw2-1.dll" /C:"libstdc++-6.dll" >nul
if not errorlevel 1 (
    echo [ERRO] A bridge x86 depende de runtime MinGW dinamico.
    goto :fail
)
echo [ARCH] Bridge PE32/i386: OK

if not exist "%INJECTOR_CPP%" (
    echo [ERRO] Source canonico do injector nao encontrado:
    echo        %INJECTOR_CPP%
    goto :fail
)

echo.
echo [BUILD] Compilando PID_Injector_x86.exe a partir do source canonico...
"%CXX%" ^
    -std=c++20 ^
    -O2 ^
    -static ^
    -static-libgcc ^
    -static-libstdc++ ^
    "%INJECTOR_CPP%" ^
    -o "%OUT_INJECTOR%"

if errorlevel 1 (
    echo.
    echo [ERRO] A compilacao do injector x86 falhou.
    goto :fail
)

if not exist "%OUT_INJECTOR%" (
    echo [ERRO] O linker terminou sem gerar:
    echo        %OUT_INJECTOR%
    goto :fail
)

echo.
echo [AUDITORIA] Verificando arquitetura e dependencias do injector...
if exist "%CXX_DIR%objdump.exe" (
    "%CXX_DIR%objdump.exe" -f "%OUT_INJECTOR%" | findstr /I /C:"pei-i386" >nul
    if errorlevel 1 (
        echo [ERRO] O injector gerado nao e PE32/i386.
        "%CXX_DIR%objdump.exe" -f "%OUT_INJECTOR%"
        goto :fail
    )
    echo [ARCH] Injector PE32/i386: OK

    echo.
    echo [DEPENDENCIAS] Injector x86:
    "%CXX_DIR%objdump.exe" -p "%OUT_INJECTOR%" | findstr /I /C:"DLL Name"

    "%CXX_DIR%objdump.exe" -p "%OUT_INJECTOR%" | findstr /I /C:"libwinpthread-1.dll" /C:"libgcc_s_dw2-1.dll" /C:"libstdc++-6.dll" >nul
    if not errorlevel 1 (
        echo.
        echo [ERRO] O injector ainda depende de runtime MinGW dinamico.
        goto :fail
    )

    > "%PKG_DIR%\BUILD_DIAGNOSTICS.txt" (
        echo ============================================================
        echo Crome Ramonia x86 - Build Diagnostics
        echo ============================================================
        echo COMPILER=%CXX%
        echo CXX_BIN=%CXX_DIR%
        echo TARGET=%TARGET%
        echo.
        echo ================= INJECTOR FILE HEADER =================
        "%CXX_DIR%objdump.exe" -f "%OUT_INJECTOR%"
        echo.
        echo ================= INJECTOR IMPORTS =====================
        "%CXX_DIR%objdump.exe" -p "%OUT_INJECTOR%" ^| findstr /I /C:"DLL Name"
        echo.
        echo ================= PROBE FILE HEADER ====================
        "%CXX_DIR%objdump.exe" -f "%OUT_DLL%"
        echo.
        echo ================= PROBE IMPORTS ========================
        "%CXX_DIR%objdump.exe" -p "%OUT_DLL%" ^| findstr /I /C:"DLL Name"
        echo.
        echo ================= RUNTIME FILES FOUND ==================
        where libgcc_s_dw2-1.dll 2^>nul
        where libstdc++-6.dll 2^>nul
        where libwinpthread-1.dll 2^>nul
    )
    echo [DIAG] %PKG_DIR%\BUILD_DIAGNOSTICS.txt
) else (
    echo [ERRO] objdump.exe nao encontrado em:
    echo        %CXX_DIR%
    goto :fail
)

echo.
echo [BUILD] Compilando cliente externo CromePipeTest_x64.exe...
set "CXX64="
if exist "C:\msys64\ucrt64\bin\g++.exe" set "CXX64=C:\msys64\ucrt64\bin\g++.exe"
if not defined CXX64 if exist "C:\msys64\mingw64\bin\g++.exe" set "CXX64=C:\msys64\mingw64\bin\g++.exe"

if not defined CXX64 (
    echo [ERRO] Compilador x64 nao encontrado.
    echo        Esperado: C:\msys64\ucrt64\bin\g++.exe
    goto :fail
)

for %%I in ("%CXX64%") do set "CXX64_DIR=%%~dpI"
set "PATH=%CXX64_DIR%;%PATH%"
for /f "delims=" %%T in ('"%CXX64%" -dumpmachine 2^>nul') do set "TARGET64=%%T"
echo [CXX64]  %CXX64%
echo [TARGET] %TARGET64%

echo %TARGET64% | findstr /I /C:"x86_64" >nul
if errorlevel 1 (
    echo [ERRO] O compilador do cliente de pipe nao e x64.
    goto :fail
)

"%CXX64%" ^
    -std=c++20 ^
    -O2 ^
    -static ^
    -static-libgcc ^
    -static-libstdc++ ^
    -I"%MINIJSON_DIR%" ^
    "%PIPE_TEST_SRC%" ^
    -o "%OUT_PIPE_TEST%"

if errorlevel 1 (
    echo [ERRO] Falha ao compilar CromePipeTest_x64.exe.
    goto :fail
)

if not exist "%OUT_PIPE_TEST%" (
    echo [ERRO] Cliente de pipe nao foi gerado.
    goto :fail
)

if exist "%CXX64_DIR%objdump.exe" (
    "%CXX64_DIR%objdump.exe" -f "%OUT_PIPE_TEST%" | findstr /I /C:"pei-x86-64" >nul
    if errorlevel 1 (
        echo [ERRO] CromePipeTest_x64.exe nao e PE32+ x86-64.
        goto :fail
    )
)
echo [ARCH] Pipe client PE32+ x86-64: OK

(
    echo Crome Ramonia x86 - Pipe Bridge V39 - read-only getTopMoveThing epilogue diagnostic
    echo.
    echo Bridge x86:
    echo   CromeRamoniaBridge_x86.dll
    echo.
    echo Cliente externo x64:
    echo   CromePipeTest_x64.exe
    echo.
    echo Profile:
    echo   profiles\ramonia_x86\6AB03F94_E281E9CA5BA0AF2.json
    echo.
    echo Teste Pipe:
    echo   1. Abra o cliente Ramonia e entre no personagem.
    echo   2. Execute PID_Injector_x86.exe.
    echo   3. Injete CromeRamoniaBridge_x86.dll.
    echo   4. Execute CromePipeTest_x64.exe.
    echo   5. Esperado: HELLO, BUILD_INFO e GET_PLAYER com [OK].
    echo.
    echo.
    echo IMPORTANTE - Cavebot:
    echo   AUTO_WALK_TO usa o contrato V40 confirmado: Directions ref + Position por valor + RET 16.
    echo   Research Probe V40: diagnostico profundo do autoWalk (RET + callsite); nao use junto do Cavebot.
    echo   Walkto repetido validado; V35 confirmou chão -> mão. V39 mantém pickup/uncover travados e coleta read-only o prólogo/epílogo completo de Tile::getTopMoveThing.
) > "%PKG_DIR%\TESTE.txt"

echo.
echo ============================================================
echo [OK] BUILD X86 CONCLUIDO
echo.
echo Pacote de teste:
echo   %PKG_DIR%
echo.
echo Research DLL:
echo   %OUT_DLL%
echo.
echo Pipe Bridge x86:
echo   %OUT_BRIDGE%
echo.
echo Pipe Test x64:
echo   %OUT_PIPE_TEST%
echo ============================================================
echo.
start "" explorer.exe "%PKG_DIR%"
pause
exit /b 0

:fail
echo.
echo ============================================================
echo [BUILD FALHOU]
echo ============================================================
echo.
pause
exit /b 1
