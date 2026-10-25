@echo off
rem Puts WSL back on Microsoft's normal kernel.
if exist "%USERPROFILE%\.wslconfig.before-hfs" (
  copy /y "%USERPROFILE%\.wslconfig.before-hfs" "%USERPROFILE%\.wslconfig" >nul
  del "%USERPROFILE%\.wslconfig.before-hfs"
) else (
  del "%USERPROFILE%\.wslconfig" 2>nul
)
wsl --shutdown
echo WSL is back on its normal kernel.
pause
