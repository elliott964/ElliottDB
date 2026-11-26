@echo off
setlocal enabledelayedexpansion

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: Visual Studio vswhere.exe not found.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSINSTALL=%%i"
)

if "%VSINSTALL%"=="" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do (
        set "VSINSTALL=%%i"
    )
)

if "%VSINSTALL%"=="" (
    echo ERROR: Visual Studio Build Tools not found.
    exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: Failed to initialize vcvars64 environment.
    exit /b 1
)

if not exist build mkdir build

echo Compiling ElliottDB (single static binary build\elliottdb.exe)...
cl /nologo /std:c11 /O2 /MT /W4 /D_CRT_SECURE_NO_WARNINGS /Isrc ^
   /Fo:build\ /Fe:build\elliottdb.exe ^
   src\edb_util.c ^
   src\pal_win.c ^
   src\config.c ^
   src\metrics.c ^
   src\pager.c ^
   src\wal.c ^
   src\index.c ^
   src\storage.c ^
   src\sql_parser.c ^
   src\sql_exec.c ^
   src\protocol.c ^
   src\reactor_win.c ^
   src\server.c ^
   src\cli.c ^
   src\bench.c ^
   src\main.c ^
   /link ws2_32.lib

if errorlevel 1 (
    echo BUILD FAILED.
    exit /b 1
)

echo BUILD SUCCEEDED: build\elliottdb.exe
