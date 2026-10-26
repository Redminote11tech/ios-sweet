@echo off
rem Copies iOS crash reports from the phone's iOS root into crashlogs\ (read-only on the phone).
rem Phone must be in TWRP and plugged in. Takes about 15 minutes (pulls the 16 GiB root).
cd /d "%~dp0"
wsl -u root -- bash -c "sed -i 's/\r$//' pull-crashlogs.sh; bash pull-crashlogs.sh 2>&1 | tee pull-crashlogs.log"
echo.
echo Finished. Tell Claude (it reads pull-crashlogs.log and crashlogs\).
pause
