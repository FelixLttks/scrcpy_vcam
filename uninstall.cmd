@echo off
rem Unregisters the scrcpy-vcam virtual camera source; needs admin rights.
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -Verb RunAs -Wait -FilePath '%~f0'"
    exit /b
)

set DEST=%ProgramData%\scrcpy-vcam
regsvr32 /u /s "%DEST%\scrcpy-vcam-source.dll"
net stop FrameServerMonitor /y >nul 2>&1
net stop FrameServer /y >nul 2>&1
rmdir /s /q "%DEST%"
echo scrcpy-vcam removed.
timeout /t 3 >nul
