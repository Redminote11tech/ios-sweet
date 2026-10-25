@echo off
rem Read-only: saves the first 8 KiB of USERDATA (iOS disk header) to hdr.bin.
cd /d "%~dp0"
adb exec-out "dd if=/dev/block/by-name/USERDATA bs=4096 count=2 2>/dev/null" > hdr.bin || goto fail
dir hdr.bin
echo Done. Tell Claude.
pause
exit /b 0
:fail
echo FAILED - is the phone in TWRP and plugged in?
pause
exit /b 1
