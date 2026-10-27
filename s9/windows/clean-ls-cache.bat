@echo off
rem Deletes iOS's LaunchServices caches in the iOS root on the phone (phone in
rem TWRP, plugged in), so lsd rebuilds them with the app extensions
rem (wallpaper posters, widgets) on the next iOS boot. Same steps as
rem root-update.bat (pulls 16 GiB, writes back only what changed).
rem Log: clean-ls-cache.log
cd /d "%~dp0"
if not exist rootfiles-ls mkdir rootfiles-ls
type nul > rootfiles-ls\install.txt
(
echo /private/var/containers/Data/System/*/Library/Caches/com.apple.LaunchServices*
echo /private/var/mobile/Containers/Data/InternalDaemon/*/Library/Caches/com.apple.LaunchServices*
echo /private/var/mobile/Library/Caches/com.apple.LaunchServices*
) > rootfiles-ls\delete.txt
echo Cleaning the LaunchServices caches (about 15-25 minutes). Leave this window open.
wsl -u root -- bash -c "sed -i 's/\r$//' root-update.sh; ROOTFILES=rootfiles-ls bash root-update.sh 2>&1 | tee clean-ls-cache.log"
echo.
echo Finished. Tell Claude (it reads clean-ls-cache.log).
pause
