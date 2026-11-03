# Installs WinDiaTweaks from its newest release, or updates it. In PowerShell:
#   irm https://github.com/zioder/WinDiaTweaks/releases/latest/download/install.ps1 | iex
# It puts WinDiaTweaks in %USERPROFILE%\WinDiaTweaks\bin, adds "Dia (Tweaks)" to the Start menu and
# desktop and opens Dia with the tweaks. Your choices in tweaks.conf are kept.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if (-not (Get-AppxPackage TheBrowserCompany.Dia)) {
    Write-Host "Dia isn't installed. Get it from https://www.diabrowser.com first."
    return
}
# Only a first install asks for a star; running this again to reinstall or update doesn't.
$firstInstall = -not (Test-Path (Join-Path $env:USERPROFILE 'WinDiaTweaks\bin\diatweaks.exe'))
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

# Optional: star the repo, only after the person presses y. Keys typed during the install are thrown
# away first so they can't answer for them; without a console to ask in, the answer is no. With a yes
# it stars through the GitHub CLI if that's signed in, otherwise it opens the repo page.
if ($firstInstall) {
    $answer = $null
    try {
        if ([Console]::IsInputRedirected -or -not [Environment]::UserInteractive) { throw 'no console' }
        $Host.UI.RawUI.FlushInputBuffer()
        Write-Host ''
        Write-Host -NoNewline 'Star WinDiaTweaks on GitHub? [y/n] '
        while ($answer -notin 'y', 'n') {
            $answer = ([string]$Host.UI.RawUI.ReadKey('NoEcho,IncludeKeyDown').Character).ToLower()
        }
        Write-Host $answer
    } catch {
        $answer = 'n'
    }
    if ($answer -eq 'y') {
        $starred = $false
        if (Get-Command gh -ErrorAction SilentlyContinue) {
            & gh auth status *> $null
            if ($LASTEXITCODE -eq 0) {
                & gh api -X PUT user/starred/zioder/WinDiaTweaks --silent *> $null
                $starred = $LASTEXITCODE -eq 0
            }
        }
        if ($starred) {
            Write-Host 'Starred. Thank you!'
        } else {
            Start-Process 'https://github.com/zioder/WinDiaTweaks'
            Write-Host 'Opened the repo page: click the Star button at the top right. Thank you!'
        }
    }
}
