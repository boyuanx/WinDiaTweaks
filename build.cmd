@echo off
rem Builds bin\DiaTweaksTap.dll (the tweak engine) and bin\diatweaks.exe (the command).
rem Needs Visual Studio 2022 (or Build Tools) with the C++ workload and a Windows 10/11 SDK.
setlocal
cd /d "%~dp0"
where cl >nul 2>nul && goto build

rem Find the C++ toolset and Windows SDK directly. (vcvars64.bat can fail on machines with a very long PATH.)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo Visual Studio C++ build tools not found & exit /b 1)
set /p VCVER=<"%VS%\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
set "VC=%VS%\VC\Tools\MSVC\%VCVER: =%"
set "SDK=%ProgramFiles(x86)%\Windows Kits\10"
for /f "delims=" %%v in ('dir /b /ad /o-n "%SDK%\Include\10.*" 2^>nul') do if not defined SV set "SV=%%v"
if not defined SV (echo Windows SDK not found & exit /b 1)
set "PATH=%VC%\bin\Hostx64\x64;%SDK%\bin\%SV%\x64;%SystemRoot%\System32;%SystemRoot%"
set "INCLUDE=%VC%\include;%SDK%\Include\%SV%\ucrt;%SDK%\Include\%SV%\um;%SDK%\Include\%SV%\shared;%SDK%\Include\%SV%\winrt"
set "LIB=%VC%\lib\x64;%SDK%\Lib\%SV%\ucrt\x64;%SDK%\Lib\%SV%\um\x64"

:build
if not exist bin mkdir bin
set FLAGS=/nologo /utf-8 /std:c++20 /O2 /EHsc /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /MT
rem C++/WinRT headers for Dia's WinUI live in gen\ (made from Dia's own .winmd files by gen-winrt.cmd).
if not exist gen\winrt\Microsoft.UI.Xaml.Controls.h call "%~dp0gen-winrt.cmd" || exit /b 1
for /f "delims=" %%v in ('dir /b /ad /o-n "%ProgramFiles(x86)%\Windows Kits\10\Include\10.*" 2^>nul') do if not defined SDKV set "SDKV=%%v"
cl %FLAGS% /bigobj /I gen /I "%ProgramFiles(x86)%\Windows Kits\10\Include\%SDKV%\cppwinrt" /LD src\tap.cpp src\ui.cpp /Fobin\ /Febin\DiaTweaksTap.dll /link /DEF:src\tap.def ole32.lib oleaut32.lib user32.lib gdi32.lib shell32.lib windowsapp.lib || exit /b 1
cl %FLAGS% src\diatweaks.cpp /Fobin\ /Febin\diatweaks.exe /link ole32.lib user32.lib shell32.lib || exit /b 1
if not exist bin\tweaks.conf copy /y tweaks.conf bin\tweaks.conf >nul
echo Built bin\diatweaks.exe and bin\DiaTweaksTap.dll
