@echo off
rem WinDiaTweaks installer: builds the add-on on this PC, adds "Dia (Tweaks)" shortcuts to the
rem Start menu and desktop, and opens Dia with the tweaks. Nothing in Dia's install folder is changed.
rem install.cmd /update is what Settings > Patches > Updates runs after git pull: no star prompt.
setlocal
if /i "%~1"=="/update" set "UPDATE=1"
rem Full path: build.cmd narrows PATH, and some PCs lack PowerShell's folder in it.
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
cd /d "%~dp0"
%PS% -NoProfile -Command "if (-not (Get-AppxPackage TheBrowserCompany.Dia)) { exit 1 }" || (echo Dia isn't installed. Get it from https://www.diabrowser.com first. & exit /b 1)

rem Updating: Dia can't have the add-on loaded while it's rebuilt, so close Dia first (it reopens with
rem the tweaks at the end). Asked politely first, so it can save its tabs.
if exist bin\DiaTweaksTap.dll %PS% -NoProfile -Command "$d = Get-Process Dia -ErrorAction SilentlyContinue; if ($d) { Write-Host 'Closing Dia to update WinDiaTweaks...'; $d | ForEach-Object { [void]$_.CloseMainWindow() }; Start-Sleep -Seconds 3; Get-Process Dia -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Seconds 1 }"

rem The helper behind "Patch Dia however it's opened" keeps bin\DiaLauncher.exe in use: stop it first.
if exist bin\diatweaks.exe bin\diatweaks.exe stop-helper

call "%~dp0build.cmd" || (echo. & echo Build failed. See "Install" in README.md for what it needs. & (if defined UPDATE pause) & exit /b 1)

rem "Dia (Tweaks)" in the Start menu and on the desktop, with the icon of the Dia on this PC.
if defined UPDATE ("%~dp0bin\diatweaks.exe" shortcuts --update) else ("%~dp0bin\diatweaks.exe" shortcuts)
"%~dp0bin\diatweaks.exe" sync-helper >nul
echo Added "Dia (Tweaks)" to the Start menu and the desktop.

"%~dp0bin\diatweaks.exe" restart
echo.
echo Done. Open Dia from "Dia (Tweaks)" in the Start menu or on the desktop from now on, then Settings ^> Patches.
if defined UPDATE (timeout /t 3 >nul & exit /b 0)

rem Optional: star the repo, only after the person presses y. Keys typed during the build are thrown
rem away first so they can't answer for them; without a console to ask in, the answer is no.
rem With a yes it stars through the GitHub CLI if that's signed in, otherwise it opens the repo page.
echo.
%PS% -NoProfile -Command "try { $Host.UI.RawUI.FlushInputBuffer() } catch { exit 1 }; Write-Host -NoNewline 'Star WinDiaTweaks on GitHub? [y/n] '; while ($true) { $k = [string]$Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown').Character; if ($k -eq 'y') { Write-Host 'y'; exit 0 }; if ($k -eq 'n') { Write-Host 'n'; exit 1 } }"
if errorlevel 1 exit /b 0
where gh >nul 2>nul || goto :starpage
gh auth status >nul 2>nul || goto :starpage
gh api -X PUT user/starred/zioder/WinDiaTweaks --silent >nul 2>nul || goto :starpage
echo Starred. Thank you!
exit /b 0
:starpage
start "" https://github.com/zioder/WinDiaTweaks
echo Opened the repo page: click the Star button at the top right. Thank you!
exit /b 0
