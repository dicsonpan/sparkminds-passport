@echo off
chcp 65001 >nul
echo ========================================================
echo         ARCS Mini Flash Tool (Full Flash: Res + App)
echo ========================================================
echo.
echo 1. Please ensure device is connected via Type-C USB cable.
echo 2. Please ensure device is POWERED ON.
echo.
echo Flashing in progress, please wait...
echo.

powershell -ExecutionPolicy Bypass -File "%~dp0adb_download.ps1" -S "%~dp0res\arcs-mini" -B "%~dp0build" -Mode default

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================================
    echo   [SUCCESS] Flashing completed! Device is rebooting...
    echo ========================================================
) else (
    echo.
    echo ========================================================
    echo   [FAILED] Flashing failed. Check USB connection/cable.
    echo ========================================================
)
echo.
pause
