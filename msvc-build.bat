@echo off
REM ============================================================================
REM tap-lang MSVC build & test helper (Windows).
REM
REM Why this exists: vcvars64.bat calls reg.exe to read the registry, and
REM reg.exe is blocked in some sandboxed shells. This script sets INCLUDE/LIB
REM manually instead, so it works from bash, cmd, or PowerShell directly.
REM
REM Usage:
REM   msvc-build.bat build          incremental build (tap.exe + tap_runtime)
REM   msvc-build.bat native-test    full run-pass/compile-fail suite (RECOMMENDED)
REM   msvc-build.bat test           same suite, fast mode (lli; unreliable here)
REM   msvc-build.bat ctest          CMake CTest (fast mode, needs lli)
REM   msvc-build.bat clean          remove build outputs
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
set "BUILD_DIR=out\build\x64-Release"

REM Headers and libraries for cl.exe (manual vcvars replacement).
set "INCLUDE=%MSVC_ROOT%\include;%WIN_KITS%\Include\%WIN_KITS_VER%\ucrt;%WIN_KITS%\Include\%WIN_KITS_VER%\um;%WIN_KITS%\Include\%WIN_KITS_VER%\shared"
set "LIB=%MSVC_ROOT%\lib\x64;%WIN_KITS%\Lib\%WIN_KITS_VER%\ucrt\x64;%WIN_KITS%\Lib\%WIN_KITS_VER%\um\x64"
REM cl.exe must be on PATH so ninja can invoke it.
set "PATH=%MSVC_ROOT%\bin\Hostx64\x64;%PATH%"

cd /d E:\projects\c-projects\tap-lang\tap

if "%1"=="build" goto build
if "%1"=="ctest" goto ctest
if "%1"=="test" goto test
if "%1"=="native-test" goto native-test
if "%1"=="clean" goto clean

echo Usage: %0 [build^|ctest^|test^|native-test^|clean]
goto end

:build
"%NINJA%" -C %BUILD_DIR% tap tap_runtime tap_runtime_shared
goto end

:ctest
"%NINJA%" -C %BUILD_DIR% test
goto end

:test
sh tests\run-tests.sh out/build/x64-Release/tap.exe
goto end

:native-test
TEST_RUN_MODE=native sh tests\run-tests.sh out/build/x64-Release/tap.exe
goto end

:clean
"%NINJA%" -C %BUILD_DIR% clean
goto end

:end
endlocal
