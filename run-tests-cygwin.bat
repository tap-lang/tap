@echo off
REM Run the tap test suite through Cygwin bash, with MSVC environment set.
REM
REM Two environment problems this script solves at once:
REM
REM 1. Git Bash's gawk mangles Windows-style temp paths inside run-tests.sh
REM    (backslash escapes get eaten, awk redirects fail, and every case then
REM    reports "missing embedded .exit expectation"). Cygwin keeps everything
REM    in native POSIX form, which is what the script expects.
REM
REM 2. In native mode tap.exe shells out to clang-cl to link each generated
REM    program. Without MSVC's INCLUDE/LIB/PATH, clang-cl reports
REM    "unable to find a Visual Studio installation" and linking fails.
REM
REM Usage:
REM   run-tests-cygwin.bat                 native mode, full suite
REM   run-tests-cygwin.bat fast            fast mode (lli; unreliable on this box)
REM   run-tests-cygwin.bat native string   native mode, only cases matching "string"

setlocal

REM --- MSVC environment (manual vcvars replacement; reg.exe is sandboxed) ---
set "MSVC_VER=14.44.35207"
set "MSVC_ROOT=D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\%MSVC_VER%"
set "WIN_KITS=D:\Windows Kits\10"
set "WIN_KITS_VER=10.0.26100.0"

set "INCLUDE=%MSVC_ROOT%\include;%WIN_KITS%\Include\%WIN_KITS_VER%\ucrt;%WIN_KITS%\Include\%WIN_KITS_VER%\um;%WIN_KITS%\Include\%WIN_KITS_VER%\shared"
set "LIB=%MSVC_ROOT%\lib\x64;%WIN_KITS%\Lib\%WIN_KITS_VER%\ucrt\x64;%WIN_KITS%\Lib\%WIN_KITS_VER%\um\x64"
set "PATH=%MSVC_ROOT%\bin\Hostx64\x64;D:\dev\LLVM\bin;%PATH%"

REM --- Cygwin-side paths (POSIX form, invisible to Git Bash conversion) ---
set "PROJECT=/cygdrive/e/projects/c-projects/tap-lang/tap"
set "TAP_EXE=out/build/x64-Release/tap.exe"

if "%1"=="fast" (
    set "MODE=fast"
) else (
    set "MODE=native"
)

if "%2"=="" (
    D:\dev\Cygwin\bin\bash.exe -lc "export TMPDIR=%PROJECT%/tmp; cd %PROJECT% && TEST_RUN_MODE=%MODE% ./tests/run-tests.sh %TAP_EXE%"
) else (
    D:\dev\Cygwin\bin\bash.exe -lc "export TMPDIR=%PROJECT%/tmp; cd %PROJECT% && TEST_RUN_MODE=%MODE% TEST_FILTER=%2 ./tests/run-tests.sh %TAP_EXE%"
)

endlocal
