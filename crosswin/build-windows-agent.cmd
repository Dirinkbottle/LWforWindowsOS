@echo off
setlocal EnableExtensions

rem CrossWin Windows Agent build script.
rem Double-click this file in Explorer, or run it from cmd.exe.
rem Requires Visual Studio 2022 (or Build Tools) with Desktop C++ and Windows SDK.

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%" || goto :pushd_failed

where cl.exe >nul 2>nul
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "%VSWHERE%" (
        echo.
        echo ERROR: MSVC was not found.
        echo Install Visual Studio 2022 or Build Tools with:
        echo   - Desktop development with C++
        echo   - MSVC x64 build tools
        echo   - Windows 10 or Windows 11 SDK
        goto :failed
    )

    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"
    if not defined VS_INSTALL (
        echo.
        echo ERROR: Visual Studio was found, but its x64 C++ tools are missing.
        goto :failed
    )

    echo Configuring Visual Studio x64 build environment...
    call "%VS_INSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
    if errorlevel 1 (
        echo.
        echo ERROR: Failed to initialize the Visual Studio x64 build environment.
        goto :failed
    )
)

if not exist build mkdir build
if errorlevel 1 (
    echo.
    echo ERROR: Could not create "%CD%\build".
    goto :failed
)

echo.
echo [1/2] Compiling shared protocol implementation...
cl /nologo /std:c11 /W4 /WX /c common\protocol.c /Fo"build\protocol.obj"
if errorlevel 1 goto :failed

echo [2/2] Compiling and linking Windows Agent...
cl /nologo /std:c++17 /W4 /WX /EHsc /Icommon /Iwindows-agent ^
  windows-agent\main.cpp ^
  windows-agent\protocol.cpp ^
  windows-agent\proxy_window.cpp ^
  build\protocol.obj ^
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
