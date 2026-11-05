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
REM   scripts\msvc-build.bat configure     one-time CMake configure (also auto-run by build)
REM   scripts\msvc-build.bat build         incremental build (tap.exe + tap_runtime + shared)
REM   scripts\msvc-build.bat rebuild       clean + configure + build
REM   scripts\msvc-build.bat test-native   full run-pass/compile-fail suite (RECOMMENDED)
REM   scripts\msvc-build.bat test          same suite, fast mode (lli; unreliable here)
REM   scripts\msvc-build.bat ctest         CMake CTest (fast mode, needs lli)
REM   scripts\msvc-build.bat package       build + bundle a redistributable folder and .zip
REM   scripts\msvc-build.bat clean         remove build outputs
REM
REM package layout (dist\tap-<ver>-win64-msvc\):
REM   bin\tap.exe  bin\LLVM-C.dll  bin\vcruntime140.dll  bin\lld-link.exe
REM   lib\tap_runtime.lib  lib\tap_runtime.dll
REM   lib\compiler-rt\clang_rt.builtins-x86_64.lib
REM   share\tap\std\*.tp
REM tap.exe derives lib\ and share\tap\std\ from its own location, so the three
REM directories must stay together. bin\lld-link.exe lets tap run without an
REM LLVM install; the Microsoft CRT import libs it needs are not bundled.
REM
REM Use test-native, not test: the lli binaries available on this machine
REM (Cygwin) misbehave with the MSVC-built tap_runtime.dll - programs exit 0
REM with no output. docs/test.md documents the same lli limitations.
REM
REM Prerequisites (already present on this machine):
REM   VS 2022 Community  D:\Program Files\Microsoft Visual Studio\2022\Community
REM   Windows SDK 10.0.26100.0     D:\Windows Kits\10
REM   LLVM-C.lib (for tap.exe)     E:\llvm-msvc-dev
REM   lld-link (links user programs; clang-cl is the fallback) D:\dev\LLVM\bin
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
REM cl.exe must be on PATH so ninja can invoke it; tap links user programs with
REM lld-link in native test mode, so its bin must be on PATH too. lld-link needs
REM the compiler-rt builtins next to it (<LLVM>\lib\clang\<ver>\lib\windows) for
REM i128 division; without them tap falls back to clang-cl.
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

REM This script lives in scripts\, so the project root is one level up. Everything
REM below uses paths relative to the root (build\, src\, std\, dist\).
cd /d "%~dp0.."

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
    echo [msvc-build] ERROR: cannot convert "%~dp0.." to a POSIX path.
    echo [msvc-build] Check that MSYS_ROOT is correct: %MSYS_ROOT%
    goto end
)

if "%1"=="configure"   goto configure
if "%1"=="build"       goto build
if "%1"=="rebuild"     goto rebuild
if "%1"=="ctest"       goto ctest
if "%1"=="test"        goto test
if "%1"=="test-native" goto test-native
if "%1"=="package"     goto package
if "%1"=="clean"       goto clean

echo Usage: %0 [configure^|build^|rebuild^|ctest^|test^|test-native^|package^|clean]
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

:test-native
call :ensure_configured
if errorlevel 1 goto end
call :build_impl
if errorlevel 1 goto end
set "TEST_RUN_MODE=native"
"%MSYS_SH%" "%PROJ_POSIX%/tests/run-tests.sh" "%PROJ_POSIX%/out/build/x64-Release/tap.exe"
goto end

:package
call :ensure_configured
if errorlevel 1 goto end
call :build_impl
if errorlevel 1 goto end
call :package_impl
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

REM ----------------------------------------------------------------------------
REM Subroutine: bundle a redistributable folder plus a .zip under dist\.
REM
REM Layout mirrors what tap expects at runtime: the compiler finds its Runtime
REM library and the standard library relative to its own location
REM (bin\..\lib and bin\..\share\tap\std), so those three directories must stay
REM together.
REM ----------------------------------------------------------------------------
:package_impl
set "TAP_VERSION="
for /f "tokens=3 delims= " %%v in ('findstr /r /c:"^#define VERSION " src\version.h.ini') do set "TAP_VERSION=%%~v"
if not defined TAP_VERSION (
    echo [msvc-build] ERROR: cannot read VERSION from src\version.h.ini
    exit /b 1
)

set "PKG_NAME=tap-%TAP_VERSION%-win64-msvc"
set "PKG_DIR=dist\%PKG_NAME%"
set "PKG_ZIP=dist\%PKG_NAME%.zip"

echo [msvc-build] Packaging %PKG_NAME% ...
if exist "%PKG_DIR%" rmdir /s /q "%PKG_DIR%"
mkdir "%PKG_DIR%\bin" 2>nul
mkdir "%PKG_DIR%\lib" 2>nul
mkdir "%PKG_DIR%\share\tap\std" 2>nul
if not exist "%PKG_DIR%\bin" (
    echo [msvc-build] ERROR: cannot create %PKG_DIR%
    exit /b 1
)

copy /y "%BUILD_DIR%\tap.exe"          "%PKG_DIR%\bin\" >nul
copy /y "%BUILD_DIR%\tap_runtime.lib"  "%PKG_DIR%\lib\" >nul
copy /y "%BUILD_DIR%\tap_runtime.dll"  "%PKG_DIR%\lib\" >nul
xcopy /y /q "std\*.tp"                 "%PKG_DIR%\share\tap\std\" >nul
copy /y "README.md"                    "%PKG_DIR%\" >nul
copy /y "LICENSE"                      "%PKG_DIR%\" >nul

REM README.md is mostly a doc index, so ship the docs it points at - otherwise
REM every link in the packaged README is dead.
mkdir "%PKG_DIR%\docs" 2>nul
xcopy /y /q "docs\*.md"                "%PKG_DIR%\docs\" >nul
mkdir "%PKG_DIR%\docs\todo" 2>nul
xcopy /y /q "docs\todo\*.md"  "%PKG_DIR%\docs\todo\" >nul

REM LLVM-C.dll is the one non-system DLL tap.exe needs. CMake copies it next to
REM the binary when TAP_LLVM_C_ROOT is set; fall back to the LLVM package itself.
if exist "%BUILD_DIR%\LLVM-C.dll" (
    copy /y "%BUILD_DIR%\LLVM-C.dll" "%PKG_DIR%\bin\" >nul
) else if exist "%LLVM_C_ROOT%\bin\LLVM-C.dll" (
    copy /y "%LLVM_C_ROOT%\bin\LLVM-C.dll" "%PKG_DIR%\bin\" >nul
) else (
    echo [msvc-build] ERROR: LLVM-C.dll not found; tap.exe cannot start without it.
    exit /b 1
)

REM VC++ runtime: the redist folder is versioned independently of the toolset
REM (e.g. 14.44.35112 next to a 14.44.35207 compiler), so scan instead of
REM hardcoding. Skipping it only means the target machine needs VC++ 2015-2022 x64.
REM MSVC_ROOT is VC\Tools\MSVC\<ver>, so the redist lives three levels up.
set "VCRUNTIME_DLL="
for /d %%d in ("%MSVC_ROOT%\..\..\..\Redist\MSVC\*") do (
    if not defined VCRUNTIME_DLL if exist "%%~d\x64\Microsoft.VC143.CRT\vcruntime140.dll" set "VCRUNTIME_DLL=%%~d\x64\Microsoft.VC143.CRT\vcruntime140.dll"
)
if defined VCRUNTIME_DLL (
    copy /y "%VCRUNTIME_DLL%" "%PKG_DIR%\bin\" >nul
) else (
    echo [msvc-build] NOTE: vcruntime140.dll not found under VC\Redist\MSVC;
    echo [msvc-build]       target machines need the VC++ 2015-2022 x64 runtime.
)

REM ---------------------------------------------------------------------------
REM Linker only. `tap run` needs a linker and the compiler-rt builtins; both are
REM LLVM (Apache-2.0 WITH LLVM-exception) and can be shipped.
REM
REM lld-link.exe from this LLVM is self-contained (static LLVM, only system
REM DLLs), so it sits right next to tap.exe. The CRT / SDK import libraries
REM (libcmt.lib, libucrt.lib, ws2_32.lib, ...) are deliberately NOT bundled:
REM they are Microsoft's and not redistributable. lld-link finds them on its own
REM through the registry, so the target machine still needs the MSVC build tools
REM or at least the Windows SDK - but no vcvars / INCLUDE / LIB setup.
set "PKG_LINKER="
if exist "%LLVM_BIN%\lld-link.exe" (
    set "PKG_LINKER=1"
) else (
    echo [msvc-build] WARNING: %LLVM_BIN%\lld-link.exe not found; shipping without a linker.
)

set "PKG_BUILTINS="
for /d %%d in ("%LLVM_BIN%\..\lib\clang\*") do (
    if not defined PKG_BUILTINS if exist "%%~d\lib\windows\clang_rt.builtins-x86_64.lib" set "PKG_BUILTINS=%%~d\lib\windows\clang_rt.builtins-x86_64.lib"
)

if defined PKG_LINKER (
    mkdir "%PKG_DIR%\lib\compiler-rt" 2>nul
    copy /y "%LLVM_BIN%\lld-link.exe" "%PKG_DIR%\bin\" >nul

    if defined PKG_BUILTINS (
        copy /y "%PKG_BUILTINS%" "%PKG_DIR%\lib\compiler-rt\clang_rt.builtins-x86_64.lib" >nul
    ) else (
        echo [msvc-build] WARNING: clang_rt.builtins-x86_64.lib not found under %LLVM_BIN%\..\lib\clang;
        echo [msvc-build]          i128 division will fail to link in the packaged compiler.
    )
)

REM Short manifest for whoever unpacks the zip.
> "%PKG_DIR%\PACKAGE.txt" echo tap %TAP_VERSION% ^(win64, MSVC^)
>>"%PKG_DIR%\PACKAGE.txt" echo.
>>"%PKG_DIR%\PACKAGE.txt" echo   bin\tap.exe            compiler; needs LLVM-C.dll next to it
>>"%PKG_DIR%\PACKAGE.txt" echo   bin\vcruntime140.dll   VC++ runtime ^(drop it if the machine has VC++ 2015-2022 x64^)
>>"%PKG_DIR%\PACKAGE.txt" echo   bin\lld-link.exe      linker used by tap run
>>"%PKG_DIR%\PACKAGE.txt" echo   lib\tap_runtime.lib    Runtime static lib, linked into every tap program
>>"%PKG_DIR%\PACKAGE.txt" echo   lib\tap_runtime.dll    Runtime DLL, loaded by lli
>>"%PKG_DIR%\PACKAGE.txt" echo   lib\compiler-rt\      clang_rt.builtins ^(i128 division helpers^)
>>"%PKG_DIR%\PACKAGE.txt" echo   share\tap\std\         standard library ^(.tp files^)
>>"%PKG_DIR%\PACKAGE.txt" echo.
>>"%PKG_DIR%\PACKAGE.txt" echo Usage:
>>"%PKG_DIR%\PACKAGE.txt" echo   bin\tap.exe --version
>>"%PKG_DIR%\PACKAGE.txt" echo   bin\tap.exe run hello.tp
>>"%PKG_DIR%\PACKAGE.txt" echo.
>>"%PKG_DIR%\PACKAGE.txt" echo tap.exe resolves lib\ and share\tap\std\ from its own location, so do not
>>"%PKG_DIR%\PACKAGE.txt" echo copy bin\tap.exe out of this layout on its own. TAP_RUNTIME_PATH and
>>"%PKG_DIR%\PACKAGE.txt" echo TAP_STD_PATH override those paths.
>>"%PKG_DIR%\PACKAGE.txt" echo.
>>"%PKG_DIR%\PACKAGE.txt" echo Requires the Universal CRT, which ships with Windows 10 and later.
>>"%PKG_DIR%\PACKAGE.txt" echo.
>>"%PKG_DIR%\PACKAGE.txt" echo The linker is bundled: tap run uses bin\lld-link.exe, so LLVM does not
>>"%PKG_DIR%\PACKAGE.txt" echo have to be installed. If it is missing, tap falls back to lld-link on
>>"%PKG_DIR%\PACKAGE.txt" echo PATH and then to clang-cl.
>>"%PKG_DIR%\PACKAGE.txt" echo.
>>"%PKG_DIR%\PACKAGE.txt" echo The CRT / Windows SDK import libraries ^(libcmt.lib, libucrt.lib,
>>"%PKG_DIR%\PACKAGE.txt" echo ws2_32.lib, ...^) are NOT bundled -- they are Microsoft's and not
>>"%PKG_DIR%\PACKAGE.txt" echo redistributable. lld-link finds them through the registry, so the target
>>"%PKG_DIR%\PACKAGE.txt" echo machine needs the MSVC build tools or the Windows SDK, but no vcvars,
>>"%PKG_DIR%\PACKAGE.txt" echo INCLUDE or LIB setup.
>>"%PKG_DIR%\PACKAGE.txt" echo.
>>"%PKG_DIR%\PACKAGE.txt" echo Size: LLVM-C.dll and lld-link.exe are 66 MB each ^(this LLVM build is
>>"%PKG_DIR%\PACKAGE.txt" echo statically linked^). tap itself is 200 KB.

REM Zip it. tar -a picks the format from the extension and is part of Windows 10+;
REM PowerShell's Compress-Archive is the fallback for older hosts.
if exist "%PKG_ZIP%" del /q "%PKG_ZIP%"
tar -a -c -f "%PKG_ZIP%" -C dist "%PKG_NAME%" 2>nul
if errorlevel 1 (
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "Compress-Archive -Path '%PKG_DIR%' -DestinationPath '%PKG_ZIP%' -Force" >nul
)

echo [msvc-build] Folder: %PKG_DIR%
if exist "%PKG_ZIP%" (
    echo [msvc-build] Zip:    %PKG_ZIP%
) else (
    echo [msvc-build] WARNING: zip creation failed; the folder above is still usable.
)
exit /b 0

:end
endlocal
