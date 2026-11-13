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
REM   scripts\msvc-build.bat install [dir] build + install into <dir> (default %LOCALAPPDATA%\tap)
REM   scripts\msvc-build.bat clean         remove build outputs
REM
REM install layout (same as `make install` / mingw-build.bat install, so no
REM environment variable is needed):
REM   <prefix>\bin\tap.exe
REM   <prefix>\bin\LLVM-C.dll               tap.exe imports it; must stay next to the exe
REM   <prefix>\bin\tap_runtime.dll          DLL next to the exe: Windows looks there first
REM   <prefix>\lib\tap_runtime.lib
REM   <prefix>\include\tap\tap_runtime.h
REM   <prefix>\share\tap\std\*.tp
REM Prefix resolution: command-line argument, then the PREFIX environment variable,
REM then %LOCALAPPDATA%\tap. Add <prefix>\bin to PATH to call `tap` from anywhere.
REM Like the MinGW install this is a development install: it does not bundle a
REM linker (bin\lld-link.exe) or compiler-rt builtins, so `tap run` still needs
REM LLVM's bin (lld-link, or the MSVC tools for the clang-cl fallback) on PATH.
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
if "%1"=="install"     goto install
if "%1"=="package"     goto package
if "%1"=="clean"       goto clean

echo Usage: %0 [configure^|build^|rebuild^|ctest^|test^|test-native^|install^|package^|clean]
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

:install
call :ensure_configured
if errorlevel 1 goto end
call :build_impl
if errorlevel 1 goto end
call :install_impl "%~2"
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
REM Subroutine: install the built compiler, Runtime and std into a prefix.
REM
REM Same layout as the Makefile's `install` target, the CMake install rules and
REM mingw-build.bat install, so the installed compiler finds everything without
REM TAP_STD_PATH / TAP_RUNTIME_PATH:
REM   bin\tap.exe             start point: <exe>/../lib and <exe>/../share/tap/std
REM   bin\LLVM-C.dll          tap.exe imports it from its own directory
REM   bin\tap_runtime.dll     loaded by lli (native programs link the static lib)
REM   lib\tap_runtime.lib     static Runtime linked into native programs
REM   include\tap\*.h         Runtime header, for programs that extend it from C
REM   share\tap\std\*.tp      standard library (prelude included)
REM
REM Only files this target owns are overwritten; anything else already under the
REM prefix is left alone. Unlike `package`, this is a development install: no
REM lld-link.exe and no compiler-rt builtins, so `tap run` still needs LLVM's bin
REM on PATH (or the MSVC tools, which the clang-cl fallback uses).
REM ----------------------------------------------------------------------------
:install_impl
set "DEST=%~1"
if not defined DEST set "DEST=%PREFIX%"
if not defined DEST set "DEST=%LOCALAPPDATA%\tap"
if not defined DEST set "DEST=%USERPROFILE%\tap"
if not defined DEST (
    echo [msvc-build] ERROR: cannot determine the install prefix.
    echo [msvc-build] Pass one explicitly: scripts\msvc-build.bat install D:\dev\tap
    exit /b 1
)

REM Normalize separators so the echoed paths never mix styles.
set "DEST=%DEST:/=\%"
REM Strip the trailing backslash but keep a drive root (D:) intact.
REM The naive comparison against a backslash literal never fires: a backslash
REM right before the closing quote reads as an escaped quote. A sentinel char
REM keeps the compared slice away from that edge.
set "CHK=%DEST%#"
if "%CHK:~-2%"=="\#" if not "%CHK:~-3%"==":\#" set "DEST=%DEST:~0,-1%"
set "CHK=%DEST%#"
if "%CHK:~-2%"=="\#" if not "%CHK:~-3%"==":\#" set "DEST=%DEST:~0,-1%"

echo [msvc-build] Installing to %DEST% ...

mkdir "%DEST%\bin" 2>nul
mkdir "%DEST%\lib" 2>nul
mkdir "%DEST%\include\tap" 2>nul
mkdir "%DEST%\share\tap\std" 2>nul
if not exist "%DEST%\bin" (
    echo [msvc-build] ERROR: cannot create "%DEST%\bin".
    echo [msvc-build] Check that the path is writable, or pass another directory:
    echo [msvc-build]   scripts\msvc-build.bat install D:\dev\tap
    exit /b 1
)

copy /y "%BUILD_DIR%\tap.exe" "%DEST%\bin\tap.exe" >nul
if errorlevel 1 goto install_copy_failed

REM LLVM-C.dll is not optional: without it tap.exe exits before main. CMake drops
REM it into the build dir next to the exe; fall back to the LLVM-C package.
set "NO_LLVM_C="
if exist "%BUILD_DIR%\LLVM-C.dll" (
    copy /y "%BUILD_DIR%\LLVM-C.dll" "%DEST%\bin\" >nul
) else if exist "%LLVM_C_ROOT%\bin\LLVM-C.dll" (
    copy /y "%LLVM_C_ROOT%\bin\LLVM-C.dll" "%DEST%\bin\" >nul
) else (
    set "NO_LLVM_C=1"
    echo [msvc-build] WARNING: LLVM-C.dll not found; tap.exe will not start
    echo [msvc-build]          unless LLVM's bin is on PATH.
)
if errorlevel 1 goto install_copy_failed

copy /y "%BUILD_DIR%\tap_runtime.dll" "%DEST%\bin\" >nul
if errorlevel 1 goto install_copy_failed
copy /y "%BUILD_DIR%\tap_runtime.lib" "%DEST%\lib\" >nul
if errorlevel 1 goto install_copy_failed
copy /y "runtime\include\tap_runtime.h" "%DEST%\include\tap\" >nul
if errorlevel 1 goto install_copy_failed

xcopy /y /q "std\*.tp" "%DEST%\share\tap\std\" >nul
if errorlevel 1 goto install_copy_failed
if exist "std\*.tap" (
    xcopy /y /q "std\*.tap" "%DEST%\share\tap\std\" >nul
    if errorlevel 1 goto install_copy_failed
)

echo [msvc-build] Installed:
echo   %DEST%\bin\tap.exe
if not defined NO_LLVM_C echo   %DEST%\bin\LLVM-C.dll
echo   %DEST%\bin\tap_runtime.dll
echo   %DEST%\lib\tap_runtime.lib
echo   %DEST%\include\tap\tap_runtime.h
echo   %DEST%\share\tap\std\*.tp

REM Self-check: a missing DLL shows up here instead of on the user's first compile.
"%DEST%\bin\tap.exe" --version >nul 2>nul
if errorlevel 1 (
    echo [msvc-build] WARNING: "%DEST%\bin\tap.exe" did not run; the install is incomplete.
) else (
    for /f "usebackq delims=" %%v in (`"%DEST%\bin\tap.exe" --version`) do (
        echo [msvc-build] Verified: %%v
    )
)

REM `tap run` shells out to a linker; it is deliberately not installed here, so say
REM where the user still has to point PATH.
if exist "%LLVM_BIN%\lld-link.exe" (
    echo [msvc-build] NOTE: `tap run` still needs a linker. Add "%LLVM_BIN%" to PATH so
    echo [msvc-build]       lld-link.exe resolves, or use `package`, which bundles it.
) else (
    echo [msvc-build] NOTE: `tap run` still needs a linker ^(lld-link, or the MSVC tools
    echo [msvc-build]       for the clang-cl fallback^); `package` bundles lld-link.exe.
)
echo [msvc-build] Add "%DEST%\bin" to PATH to run `tap` from any directory.
exit /b 0

:install_copy_failed
echo [msvc-build] ERROR: copy into "%DEST%" failed.
exit /b 1

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
