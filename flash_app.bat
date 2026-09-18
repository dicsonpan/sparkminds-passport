@echo off
chcp 65001 >nul
echo ========================================================
echo         ARCS Mini Flash Tool (Update App Firmware Only)
echo ========================================================
echo.
echo Updating app firmware...
echo.

powershell -ExecutionPolicy Bypass -File "%~dp0adb_download.ps1" -S "%~dp0res\arcs-mini" -B "%~dp0build" -Mode app

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================================
    echo   [SUCCESS] App firmware updated! Device is rebooting...
    echo ========================================================
) else (
    echo.
    echo ========================================================
    echo   [FAILED] Flashing failed. Check USB connection/cable.
    echo ========================================================
)
echo.
pause
