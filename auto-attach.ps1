[CmdletBinding()]
param([switch]$Install, [switch]$Disable)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Install -and $Disable) { throw 'Choose either -Install or -Disable.' }
$shortcut = Join-Path ([Environment]::GetFolderPath('Startup')) 'WinDiaTweaks helper.lnk'
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$script = Join-Path $PSScriptRoot 'auto-attach.ps1'
$bin = if (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'bin\diatweaks.exe')) { Join-Path $PSScriptRoot 'bin' } else { $PSScriptRoot }
$exe = Join-Path $bin 'diatweaks.exe'
$stopName = 'Local\WinDiaTweaksAutoAttachStop'

function Backup-Shortcut([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        $backup = Join-Path $bin ('backups\shortcuts\' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $backup | Out-Null
        Move-Item -LiteralPath $Path -Destination $backup
    }
}

if ($Disable) {
    try {
        $stop = [Threading.EventWaitHandle]::OpenExisting($stopName)
        $stop.Set() | Out-Null
        $stop.Dispose()
    } catch [Threading.WaitHandleCannotBeOpenedException] { }
    Backup-Shortcut $shortcut
    Write-Host 'Automatic loading disabled. The startup shortcut was moved to bin\backups\shortcuts.'
    return
}

if ($Install) {
    if (-not (Test-Path -LiteralPath $exe)) { throw 'Build diatweaks.exe before installing the helper.' }
    Backup-Shortcut $shortcut
    $link = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
    $link.TargetPath = $powershell
    $link.Arguments = '-NoProfile -NonInteractive -WindowStyle Hidden -File "' + $script + '"'
    $link.WorkingDirectory = $PSScriptRoot
    $link.WindowStyle = 7
    $link.Description = 'Load WinDiaTweaks when normal Dia opens'
    $link.Save()
    Start-Process -FilePath $powershell -ArgumentList $link.Arguments -WindowStyle Hidden
    Write-Host 'Automatic loading enabled now and at sign-in. Use the normal Dia launcher.'
    return
}

$mutex = New-Object Threading.Mutex($false, 'Local\WinDiaTweaksAutoAttach')
$owned = $false
$stop = $null
try {
    try { $owned = $mutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $owned = $true }
    if (-not $owned) { return }
    $stop = New-Object Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $stopName)
    $seen = @{}
    $log = Join-Path $bin 'auto-attach.log'
    Add-Content -LiteralPath $log -Value "$(Get-Date -Format o) Helper started."
    while (-not $stop.WaitOne(1000)) {
        $processes = @(Get-Process -Name Dia -ErrorAction SilentlyContinue)
        $keys = @($seen.Keys)
        foreach ($key in $keys) {
            if (-not ($processes | Where-Object Id -eq $key)) { $seen.Remove($key) }
        }
        foreach ($process in $processes) {
            if ($process.MainWindowHandle -eq [IntPtr]::Zero) { continue }
            $id = $process.Id
            if (-not $seen.ContainsKey($id)) { $seen[$id] = @{ Attempts = 0; Attached = $false } }
            $state = $seen[$id]
            if ($state.Attached -or $state.Attempts -ge 5) { continue }
            $state.Attempts++
            try {
                $output = & $exe attach 2>&1
                $code = $LASTEXITCODE
                if ($code -eq 0) {
                    $state.Attached = $true
                    Add-Content -LiteralPath $log -Value "$(Get-Date -Format o) Patches loaded for Dia pid $id."
                } elseif ($state.Attempts -eq 5) {
                    Add-Content -LiteralPath $log -Value "$(Get-Date -Format o) Attach failed for Dia pid $id, exit $code. Run bin\diatweaks.exe attach to see the error."
                }
            } catch {
                if ($state.Attempts -eq 5) {
                    Add-Content -LiteralPath $log -Value "$(Get-Date -Format o) Attach failed for Dia pid $id. $($_.Exception.Message)"
                }
            }
        }
    }
} finally {
    if ($stop) { $stop.Dispose() }
    if ($owned) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
