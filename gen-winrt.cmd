@echo off
rem Makes C++/WinRT headers (gen\) from the WinUI metadata shipped inside Dia, so the add-on is built
rem against exactly the WinUI version Dia runs. Re-run after a Dia update if the build complains.
rem Without Dia (or with WDT_WINMD=nuget, as in the release build) the metadata comes from NuGet.
setlocal
rem Full path: build.cmd narrows PATH, and some PCs lack PowerShell's folder in it.
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
cd /d "%~dp0"
if /i "%WDT_WINMD%"=="nuget" goto nuget
for /f "usebackq delims=" %%p in (`%PS% -NoProfile -Command "(Get-AppxPackage TheBrowserCompany.Dia).InstallLocation"`) do set "DIA=%%p"
if not defined DIA goto nuget
if not exist winmd mkdir winmd
for %%f in (Microsoft.UI.Xaml Microsoft.UI Microsoft.UI.Text Microsoft.Foundation Microsoft.Graphics Microsoft.Windows.ApplicationModel.Resources) do copy /y "%DIA%\%%f.winmd" winmd\ >nul
if not exist winmd\Microsoft.Web.WebView2.Core.winmd (
  for /d %%d in ("%USERPROFILE%\.nuget\packages\microsoft.web.webview2\*") do if exist "%%d\lib\Microsoft.Web.WebView2.Core.winmd" copy /y "%%d\lib\Microsoft.Web.WebView2.Core.winmd" winmd\ >nul
)
if not exist winmd\Microsoft.Web.WebView2.Core.winmd call :webview2
if not exist winmd\Microsoft.Web.WebView2.Core.winmd (echo Need Microsoft.Web.WebView2.Core.winmd in winmd\ - get it from the Microsoft.Web.WebView2 NuGet package & exit /b 1)

:gen
for /f "delims=" %%v in ('dir /b /ad /o-n "%ProgramFiles(x86)%\Windows Kits\10\bin\10.*" 2^>nul') do if not defined SV set "SV=%%v"
"%ProgramFiles(x86)%\Windows Kits\10\bin\%SV%\x64\cppwinrt.exe" -in winmd -ref %SV% -out gen || exit /b 1
echo Generated gen\
exit /b 0

:nuget
echo Taking WinUI's metadata from nuget.org (Dia isn't installed, or WDT_WINMD=nuget)...
%PS% -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\winmd-from-nuget.ps1" || exit /b 1
goto gen

:webview2
rem Dia's WinUI metadata refers to WebView2's; take it from Microsoft's public NuGet package.
echo Downloading Microsoft.Web.WebView2 from nuget.org (for its metadata file)...
%PS% -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $d=Join-Path $env:TEMP 'windiatweaks-webview2'; Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue; New-Item -ItemType Directory $d | Out-Null; Invoke-WebRequest 'https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.3719.77' -OutFile \"$d\p.zip\"; Expand-Archive \"$d\p.zip\" \"$d\p\"; Copy-Item \"$d\p\lib\Microsoft.Web.WebView2.Core.winmd\" 'winmd\'"
exit /b 0
