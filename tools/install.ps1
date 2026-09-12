# Copy the built DLL into a Northstar plugins directory, and prove it landed.
#
# WHY THIS IS ITS OWN STEP, AND WHY IT REFUSES TO RUN WITH THE GAME UP.
#
# Windows holds a loaded DLL open. A copy over a running game leaves the old
# build in place, and the run that follows reports results for code that was
# never loaded -- which looks exactly like a change that did not work. A whole
# headset run was spent on that once. This refuses rather than failing quietly,
# and it prints the hash on both sides so "did it land" is answered by the
# bytes and not by the absence of an error.

[CmdletBinding()]
param(
    # Northstar plugins directory. Defaults to the one recorded on the last
    # successful install, then to a search of the usual game locations.
    [string]$PluginsDir,
    # Install a specific DLL instead of the one just built.
    [string]$Dll,
    # Keep a timestamped copy of whatever is being replaced.
    [switch]$Backup
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

if (-not $Dll) { $Dll = Join-Path $repo 'plugin\build\Release\titanfall2vr.dll' }
if (-not (Test-Path $Dll)) {
    throw "No DLL at $Dll. Run tools\build.ps1 first."
}

# --- the game must be closed --------------------------------------------
$running = Get-Process -Name 'Titanfall2', 'NorthstarLauncher' -ErrorAction SilentlyContinue
if ($running) {
    $names = ($running | ForEach-Object { $_.ProcessName }) -join ', '
    throw @"
$names is running. Close the game before installing.

Windows holds the DLL open while the game is up, so this copy would silently
leave the old build in place and the next run would test code that was never
loaded. Nothing has been changed.
"@
}

# --- where to put it -----------------------------------------------------
$rememberFile = Join-Path $env:LOCALAPPDATA 'titanfall2vr\install-target.txt'

if (-not $PluginsDir -and (Test-Path $rememberFile)) {
    $remembered = (Get-Content $rememberFile -Raw).Trim()
    if ($remembered -and (Test-Path $remembered)) { $PluginsDir = $remembered }
}

if (-not $PluginsDir) {
    $guesses = @(
        'C:\Program Files (x86)\Steam\steamapps\common\Titanfall2\R2Northstar\plugins',
        'C:\Program Files\EA Games\Titanfall2\R2Northstar\plugins',
        'D:\Program Files\EA Games\Titanfall2\R2Northstar\plugins',
        'D:\Steam\steamapps\common\Titanfall2\R2Northstar\plugins'
    )
    $PluginsDir = $guesses | Where-Object { Test-Path $_ } | Select-Object -First 1
}

if (-not $PluginsDir) {
    throw @"
Could not find a Northstar plugins directory.

Pass one explicitly:
  tools\install.ps1 -PluginsDir 'X:\...\Titanfall2\R2Northstar\plugins'
"@
}
if (-not (Test-Path $PluginsDir)) { throw "No such directory: $PluginsDir" }

$target = Join-Path $PluginsDir 'titanfall2vr.dll'

# --- copy, then verify by hash ------------------------------------------
$sourceHash = (Get-FileHash -Algorithm SHA256 $Dll).Hash.ToLower()

if ($Backup -and (Test-Path $target)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    Copy-Item $target "$target.bak-$stamp"
    Write-Host "Backed up the previous DLL to titanfall2vr.dll.bak-$stamp"
}

Copy-Item $Dll $target -Force

$targetHash = (Get-FileHash -Algorithm SHA256 $target).Hash.ToLower()
if ($sourceHash -ne $targetHash) {
    throw "Copy completed but the hashes differ. source $sourceHash / target $targetHash"
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $rememberFile) | Out-Null
Set-Content -Path $rememberFile -Value $PluginsDir

Write-Host ''
Write-Host 'Installed:'
Write-Host "  $target"
Write-Host "  sha256  $targetHash   (matches the source)"
Write-Host ''

$ini = Join-Path $PluginsDir 'titanfall2vr.ini'
if (Test-Path $ini) {
    Write-Host "Config in place: $ini"
} else {
    Write-Host "No titanfall2vr.ini next to the DLL. Every default applies; copy"
    Write-Host "plugin\titanfall2vr.ini.example there to change anything."
}
