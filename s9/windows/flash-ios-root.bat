@echo off
rem Writes the iOS root image to USERDATA. ERASES everything on USERDATA.
rem Phone must be in TWRP and plugged in. Log: flash-ios-root.log
cd /d "%~dp0"
echo This ERASES the phone's USERDATA partition and writes the iOS root.
pause
wsl -u root -- bash -c "sed -i 's/\r$//' flash-ios-root.sh; bash flash-ios-root.sh 2>&1 | tee flash-ios-root.log"
echo.
echo Finished. Tell Claude (it reads flash-ios-root.log).
pause
