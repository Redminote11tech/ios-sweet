@echo off
rem Flashes big-probe3.img to the S9's BOOT partition (phone must be in TWRP).
rem To go back to Linux, run restore-linux.bat instead.
cd /d "%~dp0"
echo === Phone (should say "recovery"):
adb devices
echo === Copying big-probe3.img to the phone...
adb push big-probe3.img /tmp/ || goto fail
echo === Writing it to BOOT...
adb shell dd if=/tmp/big-probe3.img of=/dev/block/by-name/BOOT || goto fail
echo === Rebooting the phone.
adb reboot
echo Done.
pause
exit /b 0
:fail
echo.
echo FAILED - is the phone in TWRP and plugged in? Nothing was rebooted.
pause
exit /b 1
