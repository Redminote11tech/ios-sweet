@echo off
rem Read-only check of where TWRP reads on USERDATA (phone in TWRP).
cd /d "%~dp0"
adb push ios-check3.sh /tmp/ || goto fail
adb shell sh /tmp/ios-check3.sh > ios-check3.txt 2>&1
adb pull /tmp/a.bin chk-a.bin
adb pull /tmp/b.bin chk-b.bin
adb pull /tmp/c.bin chk-c.bin
adb pull /tmp/d.bin chk-d.bin
type ios-check3.txt
echo Done. Tell Claude.
pause
exit /b 0
:fail
echo FAILED - is the phone in TWRP and plugged in?
pause
exit /b 1
