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
REM   mingw-build.bat build         incremental build (tap + Runtime)
REM   mingw-build.bat rebuild       clean + build
REM   mingw-build.bat test-native   full run-pass/compile-fail suite (RECOMMENDED)
REM   mingw-build.bat test          same suite, fast mode (lli; unreliable here)
REM   mingw-build.bat clean         remove build outputs
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

cd /d "%~dp0"

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
    echo [mingw-build] ERROR: cannot convert "%~dp0" to a POSIX path.
    echo [mingw-build] Check that MSYS_ROOT is correct: %MSYS_ROOT%
    goto end
)

if "%1"=="build"       goto build
if "%1"=="rebuild"     goto rebuild
if "%1"=="test"        goto test
if "%1"=="test-native" goto test-native
if "%1"=="clean"       goto clean

echo Usage: %0 [build^|rebuild^|test^|test-native^|clean]
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

:end
endlocal
