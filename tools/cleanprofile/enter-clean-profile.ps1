# Stage a CLEAN plugins directory for the clean-install test, reversibly.
#
# RELEASE-CHECKLIST §5 asks for a clean Northstar profile, and it is right to:
# "a profile with old backups and old ini variants cannot tell you whether the
# shipped defaults work." The development directory here holds 149 files -- 50
# DLL backups, 70 ini variants, and the headset cache that makes the FIRST-LAUNCH
# path unreachable. Testing a fresh install against that proves nothing.
#
# This does not delete anything. It renames the whole directory aside and builds
# a new one containing exactly what the zip installs: two files. Undo with
# leave-clean-profile.ps1, which puts the original back byte for byte.
[CmdletBinding()]
param(
    [string]$Plugins = "D:\Program Files\EA Games\Titanfall2\R2Northstar\plugins",
    [string]$Zip     = "C:\dev\titanfall2vr\release\titanfall2vr-v0.1.1-TEST.zip"
)
$ErrorActionPreference = 'Stop'
if ((Get-Process -Name "Titanfall2*","NorthstarLauncher" -ErrorAction SilentlyContinue | Measure-Object).Count -ne 0) {
    throw "The game is running. Close it: Windows holds a loaded DLL open."
}
$parked = "$Plugins.DEV-PARKED"
if (Test-Path $parked) { throw "$parked already exists. Run leave-clean-profile.ps1 first, or move it aside by hand." }
if (-not (Test-Path $Zip)) { throw "No zip at $Zip. Run tools\package.ps1 -TestBuild." }

# Anything in there that is not ours would be another mod's plugin, and this
# would take it away with the rest. Refuse rather than guess.
$foreign = Get-ChildItem $Plugins -File | Where-Object { $_.Name -notlike 'titanfall2vr*' }
if ($foreign) {
    $foreign | ForEach-Object { Write-Host "  $($_.Name)" }
    throw "The plugins directory holds files that are not ours. Move them somewhere safe first."
}

Move-Item $Plugins $parked
New-Item -ItemType Directory -Path $Plugins | Out-Null

$staging = Join-Path ([System.IO.Path]::GetTempPath()) ("tf2vr-clean-" + [guid]::NewGuid().ToString('N'))
Expand-Archive -Path $Zip -DestinationPath $staging
$src = Get-ChildItem $staging -Directory | Select-Object -First 1
Copy-Item (Join-Path $src.FullName 'titanfall2vr.dll') $Plugins
Copy-Item (Join-Path $src.FullName 'titanfall2vr.ini') $Plugins
Remove-Item $staging -Recurse -Force

Write-Host ""
Write-Host "CLEAN PROFILE STAGED. The plugins directory now holds exactly:"
Get-ChildItem $Plugins | ForEach-Object {
    "  {0,-24} {1,9}  {2}" -f $_.Name, $_.Length, (Get-FileHash $_.FullName -Algorithm MD5).Hash
}
Write-Host ""
Write-Host "No headset cache, so the first launch derives the resolution from the runtime."
Write-Host "Your own directory is parked, untouched, at:"
Write-Host "  $parked"
Write-Host "Undo with: tools\cleanprofile\leave-clean-profile.ps1"
