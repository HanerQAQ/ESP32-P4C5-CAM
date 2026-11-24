@echo off
setlocal
cd /d "%~dp0"

echo ============================================================
echo   ESP32-P4C5-CAM Firmware Flashing Launcher
echo ============================================================

python tools\flash_firmware.py %*
if %ERRORLEVEL% neq 0 (
    echo.
    echo [ERROR] Flasher exited with error code %ERRORLEVEL%.
    pause
    exit /b %ERRORLEVEL%
)
