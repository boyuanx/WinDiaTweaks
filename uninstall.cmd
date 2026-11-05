@echo off
rem Removes the "Dia (Tweaks)" shortcuts and reopens Dia without tweaks. Then delete this folder.
setlocal
cd /d "%~dp0"
rem Stop the helper behind "Patch Dia however it's opened" and remove it from sign-in.
if exist "%~dp0diatweaks.exe" ("%~dp0diatweaks.exe" auto off >nul) else if exist "%~dp0bin\diatweaks.exe" ("%~dp0bin\diatweaks.exe" auto off >nul)
del "%APPDATA%\Microsoft\Windows\Start Menu\Programs\Dia (Tweaks).lnk" 2>nul
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -Command "Remove-Item -LiteralPath ([Environment]::GetFolderPath('Desktop') + '\Dia (Tweaks).lnk') -ErrorAction SilentlyContinue"
taskkill /IM Dia.exe /F >nul 2>nul
timeout /t 2 /nobreak >nul
start "" shell:AppsFolder\TheBrowserCompany.Dia_ttt1ap7aakyb4!Dia
echo Dia reopened without tweaks. You can now delete this folder.
