@echo off
rem Replaces rootfiles\sweetgl.dylib in the phone's iOS root in place (phone in TWRP).
rem Needs root-update.bat to have installed it once. Log: root-push-file.log
cd /d "%~dp0"
wsl -u root -- bash -c "sed -i 's/\r$//' root-push-file.sh; bash root-push-file.sh sweetgl.dylib 2>&1 | tee root-push-file.log"
echo.
echo Finished. Tell Claude (it reads root-push-file.log).
pause
