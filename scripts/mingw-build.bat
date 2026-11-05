@echo off
REM ============================================================================
REM tap-lang MinGW-w64 build & test helper (Windows).
REM
REM Why this exists: msvc-build.bat covers the MSVC toolchain; this one covers the
REM MinGW-w64 (GCC) toolchain that the Makefile targets. It puts the chosen MSYS2
REM environment on PATH, detects the installed LLVM major version (the Makefile
REM hardcodes -lLLVM-21), and drives the shell test harness with MSYS2's own sh.
REM
REM Usage:
REM   scripts\mingw-build.bat build         incremental build (tap + Runtime)
REM   scripts\mingw-build.bat rebuild       clean + build
REM   scripts\mingw-build.bat test-native   full run-pass/compile-fail suite (RECOMMENDED)
REM   scripts\mingw-build.bat test          same suite, fast mode (lli; unreliable here)
REM   scripts\mingw-build.bat package       build + bundle a self-contained folder and .zip
REM   scripts\mingw-build.bat clean         remove build outputs
REM
REM package layout (dist\tap-<ver>-win64-mingw\):
REM   bin\tap.exe  bin\ld.lld.exe
REM   bin\*.dll               collected automatically by scripts/collect-dlls.sh
REM   lib\libtap_runtime.a  lib\tap_runtime.dll
REM   lib\crt2.o crtbegin.o crtend.o default-manifest.o
REM   lib\libws2_32.a libmingw32.a libmingwex.a libmsvcrt.a libkernel32.a ...
REM   share\tap\std\*.tp
REM Unlike the MSVC package this one is fully self-contained: mingw-w64's runtime
REM is redistributable, so the CRT objects and import libs ship with it.
REM
REM Environment (all optional):
REM   MSYS_ROOT           MSYS2 installation          default: D:\dev\msys64
REM   MSYS_ENV            ucrt64 / mingw64 / clang64  default: ucrt64
REM   TAP_LLVM_VERSION    force the LLVM major version, e.g. 23 (default: detect)
REM   TEST_JOBS           test workers                default: 8
REM
REM Prerequisites:
REM   The selected MSYS2 environment needs gcc, mingw32-make and LLVM.
REM   The MSYS2 package ships mingw32-make.exe, not make.exe - hence the name.
REM ============================================================================

setlocal enabledelayedexpansion

set "MSYS_ROOT=D:\dev\msys64"
if not defined MSYS_ENV set "MSYS_ENV=ucrt64"
if not defined TEST_JOBS set "TEST_JOBS=8"
set "MSYS_BIN=%MSYS_ROOT%\%MSYS_ENV%\bin"
set "MSYS_LIB=%MSYS_ROOT%\%MSYS_ENV%\lib"
set "MSYS_SH=%MSYS_ROOT%\usr\bin\sh.exe"
set "MSYS_MAKE=%MSYS_BIN%\mingw32-make.exe"
set "TAP_EXE=build\tap.exe"

if not exist "%MSYS_MAKE%" (
    echo [mingw-build] ERROR: cannot find "%MSYS_MAKE%".
    echo [mingw-build] Point MSYS_ROOT / MSYS_ENV at your MSYS2 installation.
    goto end
)
if not exist "%MSYS_SH%" (
    echo [mingw-build] ERROR: cannot find MSYS2 sh: "%MSYS_SH%".
    echo [mingw-build] Check that MSYS_ROOT is correct: %MSYS_ROOT%
    goto end
)

REM gcc / mingw32-make first, then MSYS2's POSIX tools. The POSIX dir must come
REM before the inherited PATH so find/gawk/sed resolve to MSYS2's copies instead
REM of whatever Git Bash put there.
set "PATH=%MSYS_BIN%;%MSYS_ROOT%\usr\bin;%PATH%"

REM This script lives in scripts\, so the project root is one level up. Everything
REM below uses paths relative to the root (build\, src\, std\, dist\).
cd /d "%~dp0.."

REM The Makefile pins -lLLVM-21, but the installed LLVM is usually newer. Read the
REM major version off libLLVM-<ver>.dll.a so the build works without hand-editing
REM the Makefile. Override with TAP_LLVM_VERSION if detection guesses wrong.
set "LLVM_VERSION="
if defined TAP_LLVM_VERSION set "LLVM_VERSION=%TAP_LLVM_VERSION%"
if not defined LLVM_VERSION (
    set "LLVM_DLL="
    for /f "delims=" %%f in ('dir /b /a-d "%MSYS_LIB%\libLLVM-*.dll.a" 2^>nul') do (
        if not defined LLVM_DLL set "LLVM_DLL=%%f"
    )
    if defined LLVM_DLL (
        set "LLVM_VERSION=!LLVM_DLL:libLLVM-=!"
        set "LLVM_VERSION=!LLVM_VERSION:.dll.a=!"
    )
)
if not defined LLVM_VERSION (
    echo [mingw-build] ERROR: cannot find libLLVM-*.dll.a under "%MSYS_LIB%".
    echo [mingw-build] Install the LLVM package for %MSYS_ENV%, or set
    echo [mingw-build] TAP_LLVM_VERSION to the major version you have ^(e.g. 23^).
    goto end
)
REM MinGW has no separate libm (pow lives in the CRT); -lm is harmless and keeps
REM the line identical to the other Unix targets.
set "TAP_LIBS=-lLLVM-!LLVM_VERSION! -lm"

REM The harness compares paths from pwd against paths from find, then strips the
REM project prefix to hand tap a relative source path. A sh launched from cmd
REM inherits a Windows-style cwd, so pwd answers "E:/..." while a nested
REM "cd .. && pwd" answers "/e/..." - the strip fails and every compile-fail case
REM reports a bogus mismatch. Passing absolute POSIX paths keeps both sides in the
REM same form.
set "PROJ_POSIX="
for /f "usebackq delims=" %%i in (`"%MSYS_ROOT%\usr\bin\cygpath.exe" -a -u .`) do set "PROJ_POSIX=%%i"
if defined PROJ_POSIX if "%PROJ_POSIX:~-1%"=="/" set "PROJ_POSIX=%PROJ_POSIX:~0,-1%"
if not defined PROJ_POSIX (
    echo [mingw-build] ERROR: cannot convert "%~dp0.." to a POSIX path.
    echo [mingw-build] Check that MSYS_ROOT is correct: %MSYS_ROOT%
    goto end
)

if "%1"=="build"       goto build
if "%1"=="rebuild"     goto rebuild
if "%1"=="test"        goto test
if "%1"=="test-native" goto test-native
if "%1"=="package"     goto package
if "%1"=="clean"       goto clean

echo Usage: %0 [build^|rebuild^|test^|test-native^|package^|clean]
echo.
echo   MSYS_ENV=%MSYS_ENV%  LLVM=%LLVM_VERSION%  LIBS=%TAP_LIBS%
goto end

:build
call :build_impl
goto end

:rebuild
call :clean_impl
if errorlevel 1 goto end
call :build_impl
goto end

:test
call :build_impl
if errorlevel 1 goto end
set "TEST_RUN_MODE=fast"
call :run_suite
goto end

:test-native
call :build_impl
if errorlevel 1 goto end
set "TEST_RUN_MODE=native"
call :run_suite
goto end

:clean
call :clean_impl
goto end

:package
call :build_impl
if errorlevel 1 goto end
call :package_impl
goto end

REM ----------------------------------------------------------------------------
REM Subroutine: incremental build (tap.exe + Runtime static/shared).
REM ----------------------------------------------------------------------------
:build_impl
echo [mingw-build] Building with %MSYS_ENV% ^(LLVM %LLVM_VERSION%^) ...
REM CC=gcc must be explicit: make's built-in default is cc, so "CC ?= gcc" in the
REM Makefile never fires. LIBS overrides the hardcoded -lLLVM-21.
"%MSYS_MAKE%" CC=gcc LIBS="%TAP_LIBS%"
if errorlevel 1 exit /b 1
if not exist "%TAP_EXE%" (
    echo [mingw-build] ERROR: "%TAP_EXE%" was not produced.
    exit /b 1
)
exit /b 0

REM ----------------------------------------------------------------------------
REM Subroutine: run the shell harness with MSYS2's sh.
REM A cmd-inherited sh (e.g. Git Bash's) rebuilds TMPDIR from the Windows %TEMP%
REM with the separators stripped ("C:UserstttAppDataLocalTemp"), which makes the
REM harness write its expectation files to a broken path and report bogus failures.
REM MSYS2's sh reports a POSIX cwd and an empty TMPDIR, so the harness falls back
REM to a clean /tmp.
REM ----------------------------------------------------------------------------
:run_suite
echo [mingw-build] Testing (%TEST_RUN_MODE% mode, %TEST_JOBS% workers) ...
"%MSYS_SH%" "%PROJ_POSIX%/tests/run-tests.sh" "%PROJ_POSIX%/build/tap.exe"
goto :eof

:clean_impl
echo [mingw-build] Cleaning build outputs ...
"%MSYS_MAKE%" clean
if errorlevel 1 exit /b 1
exit /b 0

REM ----------------------------------------------------------------------------
REM Subroutine: bundle a self-contained folder + zip.
REM
REM Everything here is redistributable: mingw-w64's CRT objects and import
REM libraries are public-domain/BSD, and ld.lld + LLVM-C are Apache-2.0 WITH
REM LLVM-exception. So unlike the MSVC package this one really does run on a
REM machine with no toolchain installed.
REM ----------------------------------------------------------------------------
:package_impl
set "TAP_VERSION="
for /f "tokens=3 delims= " %%v in ('findstr /r /c:"^#define VERSION " src\version.h.ini') do set "TAP_VERSION=%%~v"
if not defined TAP_VERSION (
    echo [mingw-build] ERROR: cannot read VERSION from src\version.h.ini
    exit /b 1
)

set "PKG_NAME=tap-%TAP_VERSION%-win64-mingw"
set "PKG_DIR=dist\%PKG_NAME%"
set "PKG_ZIP=dist\%PKG_NAME%.zip"

echo [mingw-build] Packaging %PKG_NAME% ...
if exist "%PKG_DIR%" rmdir /s /q "%PKG_DIR%"
mkdir "%PKG_DIR%\bin" 2>nul
mkdir "%PKG_DIR%\lib" 2>nul
mkdir "%PKG_DIR%\share\tap\std" 2>nul
if not exist "%PKG_DIR%\bin" (
    echo [mingw-build] ERROR: cannot create %PKG_DIR%
    exit /b 1
)

copy /y "%TAP_EXE%"              "%PKG_DIR%\bin\" >nul
copy /y "build\libtap_runtime.a" "%PKG_DIR%\lib\" >nul
copy /y "build\tap_runtime.dll"  "%PKG_DIR%\lib\" >nul
xcopy /y /q "std\*.tp"           "%PKG_DIR%\share\tap\std\" >nul
copy /y "README.md"              "%PKG_DIR%\" >nul
copy /y "LICENSE"                "%PKG_DIR%\" >nul

REM README.md is mostly a doc index, so ship the docs it points at - otherwise
REM every link in the packaged README is dead.
mkdir "%PKG_DIR%\docs" 2>nul
xcopy /y /q "docs\*.md"          "%PKG_DIR%\docs\" >nul
mkdir "%PKG_DIR%\docs\todo" 2>nul
xcopy /y /q "docs\todo\*.md"  "%PKG_DIR%\docs\todo\" >nul

REM Linker. run.c prefers <exe>\ld.lld.exe over PATH, and derives the CRT
REM directory from it as <exe>\..\lib - which is exactly where the objects and
REM import libs below land.
if exist "%MSYS_BIN%\ld.lld.exe" (
    copy /y "%MSYS_BIN%\ld.lld.exe" "%PKG_DIR%\bin\" >nul
) else (
    echo [mingw-build] WARNING: %MSYS_BIN%\ld.lld.exe not found; shipping without a linker.
)

REM Collect the DLLs tap.exe and ld.lld.exe actually import, recursively. Doing
REM it by hand is a losing game: libLLVM pulls in libxml2/libffi/libiconv/zstd,
REM libstdc++ pulls in libgcc_s_seh, and missing one makes the packaged compiler
REM exit silently with code 127. The script treats anything absent from the
REM MSYS2 bin directory as a system DLL, so there is no list to maintain.
REM MSYS2 mounts its environment at /<env>, so /ucrt64/bin is the POSIX spelling
REM of %MSYS_BIN% - no cygpath round-trip needed.
set "PKG_BIN_POSIX=%PROJ_POSIX%/dist/%PKG_NAME%/bin"
set "MSYS_BIN_POSIX=/%MSYS_ENV%/bin"

echo [mingw-build] Collecting DLL dependencies ...
"%MSYS_SH%" "%PROJ_POSIX%/scripts/collect-dlls.sh" "%PKG_BIN_POSIX%" "%MSYS_BIN_POSIX%/objdump.exe" "%MSYS_BIN_POSIX%"
if errorlevel 1 (
    echo [mingw-build] ERROR: DLL collection failed.
    exit /b 1
)

REM CRT startup objects (crt2.o carries the entry point and calls __main, which
REM the Runtime provides) and the import libraries the MinGW link line names.
for %%f in (crt2.o crtbegin.o crtend.o default-manifest.o) do (
    if exist "%MSYS_LIB%\%%f" (
        copy /y "%MSYS_LIB%\%%f" "%PKG_DIR%\lib\" >nul
    ) else (
        echo [mingw-build] WARNING: %%f not found in %MSYS_LIB%
    )
)
for %%f in (libws2_32.a libmingw32.a libmingwex.a libmsvcrt.a libkernel32.a libpthread.a libadvapi32.a libshell32.a libuser32.a libucrt.a libucrtbase.a) do (
    if exist "%MSYS_LIB%\%%f" (
        copy /y "%MSYS_LIB%\%%f" "%PKG_DIR%\lib\" >nul
    ) else (
        echo [mingw-build] NOTE: %%f not found in %MSYS_LIB%; some programs may fail to link.
    )
)

REM Windows system import libs, so `-lwinmm`, `-lgdi32`, `-lole32` ... work out of
REM the box instead of forcing the user to install a toolchain. These are
REM mingw-w64's own stubs (public domain), about 8 MB in total.
set "SYS_LIBS=libwinmm.a libgdi32.a libcomdlg32.a libcomctl32.a libole32.a liboleaut32.a libuuid.a libversion.a libimm32.a libwinspool.a librpcrt4.a libsetupapi.a libcfgmgr32.a libdwmapi.a libshlwapi.a libdbghelp.a libuxtheme.a libmsimg32.a libopengl32.a libbcrypt.a libcrypt32.a libsecur32.a libiphlpapi.a libdnsapi.a libwldap32.a libnetapi32.a libmpr.a libwinhttp.a libwininet.a liburlmon.a libpsapi.a libpowrprof.a libwtsapi32.a libuserenv.a libdsound.a libdxguid.a libstrmiids.a libksuser.a libhid.a libavicap32.a"
for %%f in (%SYS_LIBS%) do (
    if exist "%MSYS_LIB%\%%f" copy /y "%MSYS_LIB%\%%f" "%PKG_DIR%\lib\" >nul
)

REM Short manifest for whoever unpacks the zip.
> "%PKG_DIR%\PACKAGE.txt" echo tap %TAP_VERSION% ^(win64, MinGW-w64 %MSYS_ENV%, LLVM !LLVM_VERSION!^)
>>"%PKG_DIR%\PACKAGE.txt" echo.
>>"%PKG_DIR%\PACKAGE.txt" echo   bin\tap.exe            compiler; needs LLVM-C next to it ^(libLLVM-!LLVM_VERSION!.dll^)
>>"%PKG_DIR%\PACKAGE.txt" echo   bin\ld.lld.exe        linker used by tap run
>>"%PKG_DIR%\PACKAGE.txt" echo   bin\*.dll             LLVM + C++ runtime DLLs, collected by walking the
>>"%PKG_DIR%\PACKAGE.txt" echo                         import tables ^(nothing to maintain by hand^)
>>"%PKG_DIR%\PACKAGE.txt" echo   lib\libtap_runtime.a  Runtime static lib, linked into every tap program
>>"%PKG_DIR%\PACKAGE.txt" echo   lib\tap_runtime.dll   Runtime DLL, loaded by lli
>>"%PKG_DIR%\PACKAGE.txt" echo   lib\*.o               CRT startup objects ^(crt2 / crtbegin / crtend^)
>>"%PKG_DIR%\PACKAGE.txt" echo   lib\*.a               MinGW import libs: the link line's own set plus
>>"%PKG_DIR%\PACKAGE.txt" echo                         common Windows ones ^(winmm, gdi32, ole32, ...^)
>>"%PKG_DIR%\PACKAGE.txt" echo   share\tap\std\        standard library ^(.tp files^)
>>"%PKG_DIR%\PACKAGE.txt" echo.
>>"%PKG_DIR%\PACKAGE.txt" echo Usage:
>>"%PKG_DIR%\PACKAGE.txt" echo   bin\tap.exe --version
>>"%PKG_DIR%\PACKAGE.txt" echo   bin\tap.exe run hello.tp
>>"%PKG_DIR%\PACKAGE.txt" echo.
>>"%PKG_DIR%\PACKAGE.txt" echo tap.exe resolves lib\ and share\tap\std\ from its own location, so do not
>>"%PKG_DIR%\PACKAGE.txt" echo copy bin\tap.exe out of this layout on its own. TAP_RUNTIME_PATH and
>>"%PKG_DIR%\PACKAGE.txt" echo TAP_STD_PATH override those paths.
>>"%PKG_DIR%\PACKAGE.txt" echo.
>>"%PKG_DIR%\PACKAGE.txt" echo This package is self-contained: bin\ld.lld.exe links your programs and
>>"%PKG_DIR%\PACKAGE.txt" echo lib\ holds the CRT objects and import libs it needs, so nothing has to
>>"%PKG_DIR%\PACKAGE.txt" echo be installed on the target machine. Requires Windows 10 or later for the
>>"%PKG_DIR%\PACKAGE.txt" echo Universal CRT.
>>"%PKG_DIR%\PACKAGE.txt" echo.
>>"%PKG_DIR%\PACKAGE.txt" echo Size is dominated by libLLVM-!LLVM_VERSION!.dll ^(about 146 MB^);
>>"%PKG_DIR%\PACKAGE.txt" echo tap itself is 200 KB.

if exist "%PKG_ZIP%" del /q "%PKG_ZIP%"
tar -a -c -f "%PKG_ZIP%" -C dist "%PKG_NAME%" 2>nul
if not exist "%PKG_ZIP%" (
    powershell -NoProfile -Command ^
        "Compress-Archive -Path '%PKG_DIR%' -DestinationPath '%PKG_ZIP%' -Force" >nul
)

echo [mingw-build] Folder: %PKG_DIR%
if exist "%PKG_ZIP%" (
    echo [mingw-build] Zip:    %PKG_ZIP%
) else (
    echo [mingw-build] WARNING: zip creation failed; the folder is still usable.
)
exit /b 0

:end
endlocal
