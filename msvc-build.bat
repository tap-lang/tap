@echo off
REM ============================================================================
REM tap-lang MSVC build & test helper (Windows).
REM
REM Why this exists: vcvars64.bat calls reg.exe to read the registry, and
REM reg.exe is blocked in some sandboxed shells. This script sets INCLUDE/LIB
REM manually instead, so it works from bash, cmd, or PowerShell directly.
REM
REM What it does: it is fully self-contained. It locates the VS-bundled cmake
REM and ninja, points CMake at the native LLVM-C package (E:\llvm-msvc-dev),
REM and auto-configures out\build\x64-Release on first use -- so you never have
REM to run cmake by hand.
REM
REM Usage:
REM   msvc-build.bat configure     one-time CMake configure (also auto-run by build)
REM   msvc-build.bat build         incremental build (tap.exe + tap_runtime + shared)
REM   msvc-build.bat rebuild       clean + configure + build
REM   msvc-build.bat native-test   full run-pass/compile-fail suite (RECOMMENDED)
REM   msvc-build.bat test          same suite, fast mode (lli; unreliable here)
REM   msvc-build.bat ctest         CMake CTest (fast mode, needs lli)
REM   msvc-build.bat clean         remove build outputs
REM
REM Use native-test, not test: the lli binaries available on this machine
REM (Cygwin) misbehave with the MSVC-built tap_runtime.dll - programs exit 0
REM with no output. docs/test.md documents the same lli limitations.
REM
REM Prerequisites (already present on this machine):
REM   VS 2022 Community  D:\Program Files\Microsoft Visual Studio\2022\Community
REM   Windows SDK 10.0.26100.0     D:\Windows Kits\10
REM   LLVM-C.lib (for tap.exe)     E:\llvm-msvc-dev
REM   clang-cl (links user programs) D:\dev\LLVM\bin
REM ============================================================================

setlocal

set "MSVC_VER=14.44.35207"
set "MSVC_ROOT=D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\%MSVC_VER%"
set "WIN_KITS=D:\Windows Kits\10"
set "WIN_KITS_VER=10.0.26100.0"
set "NINJA=D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "CMAKE=D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "LLVM_C_ROOT=E:\llvm-msvc-dev"
set "LLVM_BIN=D:\dev\LLVM\bin"
set "NINJA_DIR=D:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "MSYS_ROOT=D:\dev\msys64"
set "MSYS_SH=%MSYS_ROOT%\usr\bin\sh.exe"
set "BUILD_DIR=out\build\x64-Release"

REM Headers and libraries for cl.exe (manual vcvars replacement).
set "INCLUDE=%MSVC_ROOT%\include;%WIN_KITS%\Include\%WIN_KITS_VER%\ucrt;%WIN_KITS%\Include\%WIN_KITS_VER%\um;%WIN_KITS%\Include\%WIN_KITS_VER%\shared"
set "LIB=%MSVC_ROOT%\lib\x64;%WIN_KITS%\Lib\%WIN_KITS_VER%\ucrt\x64;%WIN_KITS%\Lib\%WIN_KITS_VER%\um\x64"
REM cl.exe must be on PATH so ninja can invoke it; clang-cl links user programs
REM in native-test mode, so put its bin on PATH too.
REM MSYS2's usr\bin goes AFTER the MSVC/LLVM entries but BEFORE the inherited
REM PATH: cl.exe must still resolve the real link.exe (MSYS ships a coreutils
REM link.exe that would hijack linking), while find/gawk/sed must resolve to
REM MSYS2's POSIX tools instead of whatever Git Bash copy is already on PATH.
set "PATH=%MSVC_ROOT%\bin\Hostx64\x64;%NINJA_DIR%;%WIN_KITS%\bin\%WIN_KITS_VER%\x64;%LLVM_BIN%;%MSYS_ROOT%\usr\bin;%PATH%"
REM The sh-based test targets run under MSYS2's sh on purpose. A cmd-inherited
REM sh (e.g. Git Bash's) rebuilds TMPDIR from the Windows %TEMP% with the
REM separators stripped ("C:UserstttAppDataLocalTemp"), which makes the harness
REM write its expectation files to a broken path and report bogus failures.
REM MSYS2's sh reports a POSIX cwd (/e/...) and an empty TMPDIR, so the harness
REM falls back to a clean /tmp - the same environment where the MinGW
REM test-native suite passes.

cd /d "%~dp0"

REM The harness compares the paths it gets from pwd against the paths find
REM reports, then strips the project prefix to hand tap a relative source path.
REM A sh launched from cmd inherits a Windows-style cwd, so pwd answers "E:/..."
REM while a nested "cd .. && pwd" answers "/e/..." - the strip fails and every
REM compile-fail case reports a bogus mismatch. Passing absolute POSIX paths
REM keeps both sides in the same form.
set "PROJ_POSIX="
for /f "usebackq delims=" %%i in (`"%MSYS_ROOT%\usr\bin\cygpath.exe" -a -u .`) do set "PROJ_POSIX=%%i"
if defined PROJ_POSIX if "%PROJ_POSIX:~-1%"=="/" set "PROJ_POSIX=%PROJ_POSIX:~0,-1%"
if not defined PROJ_POSIX (
    echo [msvc-build] ERROR: cannot convert "%~dp0" to a POSIX path.
    echo [msvc-build] Check that MSYS_ROOT is correct: %MSYS_ROOT%
    goto end
)

if "%1"=="configure"   goto configure
if "%1"=="build"       goto build
if "%1"=="rebuild"     goto rebuild
if "%1"=="ctest"       goto ctest
if "%1"=="test"        goto test
if "%1"=="native-test" goto native-test
if "%1"=="clean"       goto clean

echo Usage: %0 [configure^|build^|rebuild^|ctest^|test^|native-test^|clean]
goto end

:configure
call :configure_impl
goto end

:build
call :ensure_configured
if errorlevel 1 goto end
call :build_impl
goto end

:rebuild
if exist "%BUILD_DIR%" "%NINJA%" -C %BUILD_DIR% -t clean
call :ensure_configured
if errorlevel 1 goto end
call :build_impl
goto end

:ctest
call :ensure_configured
if errorlevel 1 goto end
call :build_impl
if errorlevel 1 goto end
"%NINJA%" -C %BUILD_DIR% test
goto end

:test
call :ensure_configured
if errorlevel 1 goto end
call :build_impl
if errorlevel 1 goto end
"%MSYS_SH%" "%PROJ_POSIX%/tests/run-tests.sh" "%PROJ_POSIX%/out/build/x64-Release/tap.exe"
goto end

:native-test
call :ensure_configured
if errorlevel 1 goto end
call :build_impl
if errorlevel 1 goto end
set "TEST_RUN_MODE=native"
"%MSYS_SH%" "%PROJ_POSIX%/tests/run-tests.sh" "%PROJ_POSIX%/out/build/x64-Release/tap.exe"
goto end

:clean
if exist "%BUILD_DIR%" "%NINJA%" -C %BUILD_DIR% clean
goto end

REM ----------------------------------------------------------------------------
REM Subroutine: configure the build directory with CMake (native LLVM-C).
REM ----------------------------------------------------------------------------
:configure_impl
echo [msvc-build] Configuring %BUILD_DIR% ...
"%CMAKE%" -S . -B %BUILD_DIR% -G Ninja ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DTAP_LLVM_C_ROOT=%LLVM_C_ROOT% ^
    -DBUILD_TESTS=ON
if errorlevel 1 exit /b 1
exit /b 0

REM ----------------------------------------------------------------------------
REM Subroutine: configure only if build.ninja is missing (auto-run by targets).
REM ----------------------------------------------------------------------------
:ensure_configured
if not exist "%BUILD_DIR%\build.ninja" (
    call :configure_impl
    if errorlevel 1 exit /b 1
)
exit /b 0

REM ----------------------------------------------------------------------------
REM Subroutine: build the three products every test target needs.
REM ----------------------------------------------------------------------------
:build_impl
"%NINJA%" -C %BUILD_DIR% tap tap_runtime tap_runtime_shared
if errorlevel 1 exit /b 1
exit /b 0

:end
endlocal
