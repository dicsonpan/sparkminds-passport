@echo off
setlocal
cd /d "%~dp0"
echo ========================================================
echo        ARCS Mini Flash Tool (Full Flash: Res + App)
echo ========================================================
echo.
echo 1. Please ensure device is connected via USB cable.
echo 2. Please ensure device is turned ON.
echo.
echo Running flash script...
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
    echo   [FAILED] Flashing failed.
    echo ========================================================
)
echo.
pause
