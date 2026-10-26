@echo off
rem Finishes a root-update.bat that stopped while writing back (phone in TWRP).
rem Uses the updated copy already in WSL: no new 16 GiB pull. Log: root-update.log
cd /d "%~dp0"
wsl -u root -- bash -c "sed -i 's/\r$//' root-update.sh; bash root-update.sh --writeback 2>&1 | tee root-update.log"
echo.
echo Finished. Tell Claude (it reads root-update.log).
pause
