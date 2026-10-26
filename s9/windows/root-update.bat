@echo off
rem Adds the files in rootfiles\install.txt to the iOS root on the phone
rem (phone in TWRP, plugged in). Pulls 16 GiB, writes back only what changed.
rem Log: root-update.log
cd /d "%~dp0"
echo Updating the iOS root on the phone (about 15-25 minutes). Leave this window open.
wsl -u root -- bash -c "sed -i 's/\r$//' root-update.sh; bash root-update.sh 2>&1 | tee root-update.log"
echo.
echo Finished. Tell Claude (it reads root-update.log).
pause
