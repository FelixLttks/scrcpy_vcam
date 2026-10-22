@echo off
rem Registers the scrcpy-vcam virtual camera source. Run once after building; needs admin rights.
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -Verb RunAs -Wait -FilePath '%~f0'"
    exit /b
)

if not exist "%~dp0bin\scrcpy-vcam-source.dll" (
    echo bin\scrcpy-vcam-source.dll not found. Run build.cmd first.
    pause
    exit /b 1
)

set DEST=%ProgramData%\scrcpy-vcam
if not exist "%DEST%" mkdir "%DEST%"
rem The Windows Camera Frame Server (LOCAL SERVICE) must be able to read the DLL and write its log.
icacls "%DEST%" /grant *S-1-1-0:(OI)(CI)M >nul

rem Release a previously loaded copy of the DLL.
net stop FrameServerMonitor /y >nul 2>&1
net stop FrameServer /y >nul 2>&1

copy /y "%~dp0bin\scrcpy-vcam-source.dll" "%DEST%\scrcpy-vcam-source.dll" >nul || goto :fail
regsvr32 /s "%DEST%\scrcpy-vcam-source.dll" || goto :fail
echo scrcpy-vcam installed: %DEST%\scrcpy-vcam-source.dll
timeout /t 3 >nul
exit /b 0

:fail
echo Installation failed.
pause
exit /b 1
