@echo off
rem 1) puts rootfiles\sweetgl.dylib into the phone's iOS root, 2) flashes ios-pb1.img, 3) reboots.
rem Phone must be in TWRP and plugged in.
cd /d "%~dp0"
echo === Phone (should say "recovery"):
adb devices
echo === Updating sweetgl.dylib in the iOS root...
wsl -u root -- bash -c "sed -i 's/\r$//' root-push-file.sh; bash root-push-file.sh sweetgl.dylib 2>&1 | tee root-push-file.log"
findstr /c:"RESULT: FILE_UPDATED" root-push-file.log >nul || goto fail
echo === Copying ios-pb1.img to the phone...
adb push ios-pb1.img /tmp/ || goto fail
echo === Writing it to BOOT...
adb shell dd if=/tmp/ios-pb1.img of=/dev/block/by-name/BOOT || goto fail
echo === Rebooting the phone.
adb reboot
echo Done.
pause
exit /b 0
:fail
echo.
echo FAILED - nothing was rebooted. Tell Claude (it reads root-push-file.log).
pause
exit /b 1
