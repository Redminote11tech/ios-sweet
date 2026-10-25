@echo off
rem Read-only check of the iOS disk on the phone (phone in TWRP).
cd /d "%~dp0"
adb push ios-check.sh /tmp/ || goto fail
adb shell sh /tmp/ios-check.sh > ios-check.txt
type ios-check.txt
echo Done. Tell Claude.
pause
exit /b 0
:fail
echo FAILED - is the phone in TWRP and plugged in?
pause
exit /b 1
