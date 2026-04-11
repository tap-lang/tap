@echo off
title Version Generator

REM Default commit ID
set GIT_COMMIT_ID=dev

REM Try to get git commit ID if git exists
where git >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    for /f "delims=" %%i in ('git log -1 --pretty^=format:%%h 2^>nul') do (
        set GIT_COMMIT_ID=%%i
    )
)


REM Get the directory where this script is located
set "SRC_DIR=%~dp0..\"

REM Copy version.h.ini to version.h using full paths
copy "%SRC_DIR%version.h.ini" "%SRC_DIR%version.h" >nul 2>nul

REM Replace @GIT_COMMIT_ID placeholder with actual commit ID
setlocal enabledelayedexpansion
> "%SRC_DIR%version.h.new" (
    for /f "usebackq delims=" %%a in ("%SRC_DIR%version.h") do (
        set "line=%%a"
        set "line=!line:@GIT_COMMIT_ID=%GIT_COMMIT_ID%!"
        echo !line!
    )
)
del "%SRC_DIR%version.h"
ren "%SRC_DIR%version.h.new" "version.h"
endlocal