@echo off
rem Puts your original Linux boot image (bk\BOOT.img) back on the S9 (phone must be in TWRP).
cd /d "%~dp0"
adb devices
adb push bk\BOOT.img /tmp/ || goto fail
adb shell dd if=/tmp/BOOT.img of=/dev/block/by-name/BOOT || goto fail
adb reboot
echo Done.
pause
exit /b 0
:fail
echo FAILED - is the phone in TWRP and plugged in?
pause
exit /b 1
