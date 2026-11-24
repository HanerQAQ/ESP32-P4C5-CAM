@echo off
cd /d "%~dp0"
echo ===================================================
echo Building ColorTool_v1.0.exe with PyInstaller...
echo ===================================================
python -m PyInstaller --noconfirm --clean ColorTool_v1.0.spec
if errorlevel 1 (
    echo [ERROR] Build failed!
    pause
    exit /b 1
)
copy /y "dist\ColorTool_v1.0.exe" "ColorTool_v1.0.exe"
echo ===================================================
echo [SUCCESS] ColorTool_v1.0.exe generated successfully!
echo ===================================================
pause
