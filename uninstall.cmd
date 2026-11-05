@echo off
rem Removes the "Dia (Tweaks)" shortcuts and reopens Dia without tweaks. Then delete this folder.
setlocal
cd /d "%~dp0"
if exist "%~dp0auto-attach.ps1" "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -File "%~dp0auto-attach.ps1" -Disable || exit /b 1
del "%APPDATA%\Microsoft\Windows\Start Menu\Programs\Dia (Tweaks).lnk" 2>nul
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -Command "Remove-Item -LiteralPath ([Environment]::GetFolderPath('Desktop') + '\Dia (Tweaks).lnk') -ErrorAction SilentlyContinue"
taskkill /IM Dia.exe /F >nul 2>nul
timeout /t 2 /nobreak >nul
start "" shell:AppsFolder\TheBrowserCompany.Dia_ttt1ap7aakyb4!Dia
echo Dia reopened without tweaks. You can now delete this folder.
