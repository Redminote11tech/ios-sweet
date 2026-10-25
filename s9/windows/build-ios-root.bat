@echo off
rem Builds the iOS 16 root image in WSL (about 30-60 minutes, ~5 GB download).
rem Safe to run again: it continues where it stopped. Log: ios-root.log
cd /d "%~dp0"
echo Building the iOS root in WSL. Leave this window open.
wsl -u root -- bash -c "sed -i 's/\r$//' build-ios-root.sh; bash build-ios-root.sh 2>&1 | tee ios-root.log"
echo.
echo Finished. Tell Claude (it reads ios-root.log).
pause
