@echo off
rem Reads the iOS kernel log saved on the phone (phone in TWRP). Read-only.
cd /d "%~dp0"
adb devices
adb push sweetlog-area.sh /tmp/ || goto fail
adb exec-out sh /tmp/sweetlog-area.sh > sweetlog.bin || goto fail
dir sweetlog.bin
echo Done. Tell Claude.
pause
exit /b 0
:fail
echo FAILED - is the phone in TWRP and plugged in?
pause
exit /b 1
