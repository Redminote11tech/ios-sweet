@echo off
rem Counts LSE atomics in the iOS libraries (read-only). Log: scan-lse.log
cd /d "%~dp0"
wsl -u root -- bash -c "sed -i 's/\r$//' scan-lse.sh; bash scan-lse.sh 2>&1 | tee scan-lse.log"
echo Finished. Tell Claude.
pause
