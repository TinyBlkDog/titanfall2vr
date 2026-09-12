# Put the development plugins directory back exactly as it was.
[CmdletBinding()]
param([string]$Plugins = "D:\Program Files\EA Games\Titanfall2\R2Northstar\plugins")
$ErrorActionPreference = 'Stop'
if ((Get-Process -Name "Titanfall2*","NorthstarLauncher" -ErrorAction SilentlyContinue | Measure-Object).Count -ne 0) {
    throw "The game is running. Close it first."
}
$parked = "$Plugins.DEV-PARKED"
if (-not (Test-Path $parked)) { throw "Nothing parked at $parked -- there is nothing to restore." }

# The clean directory is disposable by construction: two files that came out of
# the zip, plus whatever the test run wrote (a headset cache, a log). Keep it
# next to the parked one rather than deleting it, so a test run's cache and log
# survive to be read afterwards.
$stamp = Get-Date -Format 'yyyy-MM-dd-HHmmss'
if (Test-Path $Plugins) { Move-Item $Plugins "$Plugins.CLEANTEST-$stamp" }
Move-Item $parked $Plugins
Write-Host ""
Write-Host "Restored. The plugins directory holds $((Get-ChildItem $Plugins -File).Count) files again."
Write-Host ("  your ini: " + (Get-FileHash "$Plugins\titanfall2vr.ini" -Algorithm MD5).Hash)
Write-Host "The clean-test directory, with whatever that run wrote, is kept at:"
Write-Host "  $Plugins.CLEANTEST-$stamp"
