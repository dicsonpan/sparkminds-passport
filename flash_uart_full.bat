@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo ========================================================
echo   ARCS Mini Full Flash Tool (All 7 Partitions via UART)
echo ========================================================
echo.
echo Port: COM4 (Baud: 1500000)
echo Partitions:
echo   [0] 0x03E000 -> boot_control_clean.bin (清空OTA死循环标记)
echo   [1] 0x000000 -> boot.bin
echo   [2] 0x040000 -> ap.bin
echo   [3] 0x100000 -> tone.bin
echo   [4] 0x200000 -> wake_word.bin
echo   [5] 0x380000 -> emoji.bin
echo   [6] 0x440000 -> respak.bin
echo   [7] 0x600000 -> build\arcs-mini.bin (Student: 陆昭闻 Owen SM-2026-002)
echo   [8] 0x0A00000 -> ota_clean.bin (清空残留OTA包)
echo.
echo If prompted, click device RESET (RST) button to start flashing.
echo.

".\tools\cskburn\cskburn.exe" -C arcs -s COM4 -b 1500000 --probe-timeout 45000 0x03E000 res\arcs-mini\boot_control_clean.bin 0x000000 res\arcs-mini\boot.bin 0x040000 res\arcs-mini\ap.bin 0x100000 res\arcs-mini\tone.bin 0x200000 res\arcs-mini\wake_word.bin 0x380000 res\arcs-mini\emoji.bin 0x440000 res\arcs-mini\respak.bin 0x600000 build\arcs-mini.bin 0x0A00000 res\arcs-mini\ota_clean.bin

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================================
    echo   [SUCCESS] Full flash completed! Device is rebooting...
    echo ========================================================
) else (
    echo.
    echo ========================================================
    echo   [FAILED] Flash failed. Please check COM4 connection.
    echo ========================================================
)
echo.
pause
