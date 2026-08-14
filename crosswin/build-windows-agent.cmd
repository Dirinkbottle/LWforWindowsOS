@echo off
setlocal EnableExtensions

rem CrossWin Windows Agent build script.
rem Double-click this file in Explorer, or run it from cmd.exe.
rem Requires Visual Studio 2022 (or Build Tools) with Desktop C++ and Windows SDK.

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%" || goto :pushd_failed

where cl.exe >nul 2>nul
if errorlevel 1 (
    rem Explorer starts cmd.exe without the VS developer environment. Prefer
    rem vswhere, then try the normal VS 2022/2019 installation directories.
    set "VSDEVCMD="
    set "VS_INSTALL="
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "%VSWHERE%" (
        for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"
        if defined VS_INSTALL if exist "%VS_INSTALL%\Common7\Tools\VsDevCmd.bat" set "VSDEVCMD=%VS_INSTALL%\Common7\Tools\VsDevCmd.bat"
    )

    for %%I in (
        "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\Common7\Tools\VsDevCmd.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\Common7\Tools\VsDevCmd.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional\Common7\Tools\VsDevCmd.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise\Common7\Tools\VsDevCmd.bat"
    ) do if not defined VSDEVCMD if exist "%%~fI" set "VSDEVCMD=%%~fI"

    if not defined VSDEVCMD goto :vs_environment_missing

    echo Configuring Visual Studio x64 build environment...
    call "%VSDEVCMD%" -no_logo -arch=x64 -host_arch=x64
    if errorlevel 1 goto :vs_environment_failed
    where cl.exe >nul 2>nul
    if errorlevel 1 goto :vs_environment_failed
)

if not exist build mkdir build
if errorlevel 1 (
    echo.
    echo ERROR: Could not create "%CD%\build".
    goto :failed
)

echo.
echo [1/3] Compiling shared protocol implementation...
cl /nologo /std:c11 /W4 /WX /c common\protocol.c /Fo"build\protocol.obj"
if errorlevel 1 goto :failed

echo [2/3] Compiling shared Geometry Oracle...
cl /nologo /std:c11 /W4 /WX /c ..\geometry\geometry.c /Fo"build\geometry.obj"
if errorlevel 1 goto :failed

echo [3/3] Compiling and linking Windows Agent...
cl /nologo /std:c++17 /W4 /WX /EHsc /Icommon /Iwindows-agent /I..\geometry ^
  windows-agent\main.cpp ^
  windows-agent\protocol.cpp ^
  windows-agent\proxy_window.cpp ^
  build\protocol.obj ^
  build\geometry.obj ^
  /link ws2_32.lib user32.lib gdi32.lib ^
  /out:build\crosswin-agent.exe
if errorlevel 1 goto :failed

echo.
echo ============================================================
echo BUILD SUCCESS
echo Output: %CD%\build\crosswin-agent.exe
echo ============================================================
echo.
echo Example:
echo   build\crosswin-agent.exe --host 192.168.122.1 --port 44600 --trace-protocol --trace-present --trace-input --trace-frame --trace-damage
goto :done

:pushd_failed
echo ERROR: Unable to open the script directory.
goto :failed

:vs_environment_missing
echo.
echo ERROR: MSVC x64 environment was not found.
echo Run from a "x64 Native Tools Command Prompt for VS", or install Visual Studio
echo / Build Tools with Desktop C++, MSVC x64, and a Windows SDK.
goto :failed

:vs_environment_failed
echo.
echo ERROR: Visual Studio was found, but its x64 compiler environment could not be initialized.
goto :failed

:failed
echo.
echo ============================================================
echo BUILD FAILED
echo ============================================================

:done
popd
echo.
pause
endlocal
