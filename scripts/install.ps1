# Installs WinDiaTweaks from its newest release, or updates it. In PowerShell:
#   irm https://github.com/zioder/WinDiaTweaks/releases/latest/download/install.ps1 | iex
# It puts WinDiaTweaks in %USERPROFILE%\WinDiaTweaks\bin, adds "Dia (Tweaks)" to the Start menu and
# opens Dia with the tweaks. Your choices in tweaks.conf are kept.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if (-not (Get-AppxPackage TheBrowserCompany.Dia)) {
    Write-Host "Dia isn't installed. Get it from https://www.diabrowser.com first."
    return
}
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("WinDiaTweaks-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $tmp | Out-Null
try {
    Write-Host 'Downloading WinDiaTweaks...'
    Invoke-WebRequest 'https://github.com/zioder/WinDiaTweaks/releases/latest/download/WinDiaTweaks.zip' -OutFile "$tmp\WinDiaTweaks.zip" -UseBasicParsing
    Expand-Archive "$tmp\WinDiaTweaks.zip" "$tmp\files"
    & "$tmp\files\diatweaks.exe" install
    if ($LASTEXITCODE) { throw "WinDiaTweaks didn't install (exit code $LASTEXITCODE)." }
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
