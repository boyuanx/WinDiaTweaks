# Puts the WinUI metadata Dia is built with into winmd\, taken from Microsoft's public NuGet packages
# instead of an installed Dia. Used by the release build (no Dia there) and when Dia isn't installed.
# The versions are the Windows App SDK that Dia ships (1.5.x uses 2.3); WinRT interfaces never change
# once published, so a build against these runs on later Dia versions too.
param([string]$Out = (Join-Path $PSScriptRoot '..\winmd'))
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$packages = @(
    @{ Id = 'Microsoft.WindowsAppSDK.WinUI'; Version = '2.3.0'
       Files = 'metadata/Microsoft.UI.Xaml.winmd', 'metadata/Microsoft.UI.Text.winmd' },
    @{ Id = 'Microsoft.WindowsAppSDK.InteractiveExperiences'; Version = '2.1.3'
       Files = 'metadata/10.0.18362.0/Microsoft.UI.winmd', 'metadata/10.0.18362.0/Microsoft.Foundation.winmd', 'metadata/10.0.18362.0/Microsoft.Graphics.winmd' },
    @{ Id = 'Microsoft.WindowsAppSDK.Foundation'; Version = '2.3.5'
       Files = 'metadata/Microsoft.Windows.ApplicationModel.Resources.winmd' },
    @{ Id = 'Microsoft.Web.WebView2'; Version = '1.0.3719.77'
       Files = 'lib/Microsoft.Web.WebView2.Core.winmd' }
)
New-Item -ItemType Directory -Force $Out | Out-Null
$tmp = Join-Path ([IO.Path]::GetTempPath()) "windiatweaks-nuget-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    foreach ($p in $packages) {
        $id = $p.Id.ToLower()
        $zip = Join-Path $tmp "$id.zip"
        Write-Host "Downloading $($p.Id) $($p.Version) from nuget.org..."
        Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/$id/$($p.Version)/$id.$($p.Version).nupkg" -OutFile $zip -UseBasicParsing
        $archive = [IO.Compression.ZipFile]::OpenRead($zip)
        try {
            foreach ($f in $p.Files) {
                $entry = $archive.Entries | Where-Object FullName -eq $f
                if (-not $entry) { throw "$f is missing from $($p.Id) $($p.Version)" }
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $Out $entry.Name), $true)
            }
        } finally { $archive.Dispose() }
    }
} finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
