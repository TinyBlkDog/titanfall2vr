# Remove titanfall2vr from a Northstar plugins directory.
#
# It removes only files this mod put there, it names every one before deleting
# it, and it leaves your configuration alone unless you ask for it to go too.

[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$PluginsDir,
    # Also delete titanfall2vr.ini and the per-headset cache. Off by default:
    # an ini is a tuned configuration someone spent time on, and reinstalling
    # is not a reason to lose it.
    [switch]$IncludeConfig
)

$ErrorActionPreference = 'Stop'

$rememberFile = Join-Path $env:LOCALAPPDATA 'titanfall2vr\install-target.txt'
if (-not $PluginsDir -and (Test-Path $rememberFile)) {
    $remembered = (Get-Content $rememberFile -Raw).Trim()
    if ($remembered -and (Test-Path $remembered)) { $PluginsDir = $remembered }
}
if (-not $PluginsDir) {
    throw "Pass -PluginsDir 'X:\...\Titanfall2\R2Northstar\plugins'."
}
if (-not (Test-Path $PluginsDir)) { throw "No such directory: $PluginsDir" }

$running = Get-Process -Name 'Titanfall2', 'NorthstarLauncher' -ErrorAction SilentlyContinue
if ($running) {
    throw "Close the game first. Windows holds the DLL open while it is running."
}

$names = @('titanfall2vr.dll')
if ($IncludeConfig) { $names += @('titanfall2vr.ini', 'titanfall2vr.headset.cache') }

$found = foreach ($name in $names) {
    $path = Join-Path $PluginsDir $name
    if (Test-Path $path) { Get-Item $path }
}

if (-not $found) {
    Write-Host "Nothing to remove in $PluginsDir."
    if (-not $IncludeConfig) {
        Write-Host "(Config files are left alone unless you pass -IncludeConfig.)"
    }
    return
}

Write-Host "About to remove from $PluginsDir :"
$found | ForEach-Object { Write-Host "  $($_.Name)" }

foreach ($item in $found) {
    if ($PSCmdlet.ShouldProcess($item.FullName, 'Remove')) {
        Remove-Item $item.FullName -Force
    }
}

$backups = Get-ChildItem -Path $PluginsDir -Filter 'titanfall2vr.dll.bak-*' -ErrorAction SilentlyContinue
if ($backups) {
    Write-Host ''
    Write-Host "$($backups.Count) backup DLL(s) left in place. Delete them yourself if you want them gone."
}

if (-not $IncludeConfig) {
    Write-Host ''
    Write-Host 'titanfall2vr.ini was kept. Pass -IncludeConfig to remove it too.'
}
