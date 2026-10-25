@echo off
rem Makes WSL start with the kernel that has HFS+ (bzImage in this folder).
rem Undo any time with stock-wsl-kernel.bat. Log: wsl-kernel.log
cd /d "%~dp0"
if not exist bzImage ( echo bzImage missing in this folder & pause & exit /b 1 )
mkdir "%USERPROFILE%\wsl-hfs-kernel" 2>nul
copy /y bzImage "%USERPROFILE%\wsl-hfs-kernel\bzImage" >nul
if exist "%USERPROFILE%\.wslconfig" if not exist "%USERPROFILE%\.wslconfig.before-hfs" copy /y "%USERPROFILE%\.wslconfig" "%USERPROFILE%\.wslconfig.before-hfs" >nul
set "K=%USERPROFILE:\=\\%\\wsl-hfs-kernel\\bzImage"
(
echo [wsl2]
echo kernel=%K%
) > "%USERPROFILE%\.wslconfig"
echo Restarting WSL with the new kernel...
wsl --shutdown
timeout /t 8 /nobreak >nul
wsl -u root -- bash -c "uname -r; grep -w hfsplus /proc/filesystems && echo HFSPLUS_OK || echo NO_HFSPLUS" > wsl-kernel.log 2>&1
type wsl-kernel.log
echo.
echo Done. Now double-click build-ios-root.bat again.
pause
