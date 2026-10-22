@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem Find Visual Studio (2019 or newer, any edition or Build Tools) with the C++ toolset.
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" (
    echo vswhere.exe not found. Install Visual Studio or the Build Tools with "Desktop development with C++".
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
    echo No Visual Studio installation with the C++ toolset found.
    exit /b 1
)

rem Pick the newest Windows SDK that has mfvirtualcamera.h (Windows 11 SDK, 10.0.22000.0 or newer).
set SDKINC=%ProgramFiles(x86)%\Windows Kits\10\Include
set SDKVER=
for /f "delims=" %%v in ('dir /b /ad /o-n "%SDKINC%" 2^>nul') do (
    if not defined SDKVER if exist "%SDKINC%\%%v\um\mfvirtualcamera.h" set SDKVER=%%v
)
if not defined SDKVER (
    echo No Windows 11 SDK found ^(needs um\mfvirtualcamera.h^). Install the Windows 11 SDK.
    exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" %SDKVER% >nul || exit /b 1

if not exist bin mkdir bin
if not exist obj mkdir obj

set CFLAGS=/nologo /O2 /MT /EHsc /std:c++17 /W3 /DUNICODE /D_UNICODE /Fo:obj\

cl %CFLAGS% /LD src\source.cpp /Fe:bin\scrcpy-vcam-source.dll /link /DEF:src\source.def || exit /b 1
cl %CFLAGS% src\scrcpy-vcam.cpp /Fe:bin\scrcpy-vcam.exe || exit /b 1
cl %CFLAGS% src\testcap.cpp /Fe:bin\testcap.exe || exit /b 1
echo.
echo Build OK (Windows SDK %SDKVER%): bin\scrcpy-vcam.exe, bin\scrcpy-vcam-source.dll, bin\testcap.exe
