# Build the release archive.
#
# The version is read out of plugin/CMakeLists.txt, which is the single place
# it is declared, so the zip name, the DLL's own startup banner and the git tag
# cannot disagree. There is no -Version parameter on purpose: one would be a
# second place to get it wrong.
#
# This packages what is on disk. It does not build -- run tools\build.ps1 first
# and hand it a DLL you have actually tested.

[CmdletBinding()]
param(
    [string]$Dll,
    [string]$Ini,
    [string]$OutputDirectory,
    # Skip the documentation placeholder gate and mark the archive as a TEST
    # build. For trying the shipping artefact before the README is finished --
    # the name makes it impossible to mistake for a release.
    [switch]$TestBuild
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# --- the version, from the one place it is declared ----------------------
$cmakeLists = Join-Path $repo 'plugin\CMakeLists.txt'
$versionLine = Select-String -Path $cmakeLists -Pattern '^\s*set\(TF2VR_VERSION_TAG\s+"([^"]+)"\)' |
    Select-Object -First 1
if (-not $versionLine) {
    throw "Could not read TF2VR_VERSION_TAG from $cmakeLists. That line is the only place the version exists; fix it there."
}
$tag = $versionLine.Matches[0].Groups[1].Value
Write-Host "version: $tag"

# --- inputs ---------------------------------------------------------------
if (-not $Dll) { $Dll = Join-Path $repo 'plugin\build\Release\titanfall2vr.dll' }
if (-not (Test-Path $Dll)) { throw "No DLL at $Dll. Run tools\build.ps1 first." }

if (-not $Ini) { $Ini = Join-Path $repo 'plugin\titanfall2vr.ini.example' }
if (-not (Test-Path $Ini)) { throw "No ini at $Ini." }

if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo 'release' }

# --- refuse to ship a placeholder ----------------------------------------
$docs = @('README.md','INSTALL.md','CONTROLS.md','LICENSE','CHANGELOG.md',
          'KNOWN-ISSUES.md','THIRD_PARTY_NOTICES.md','BUILDING.md') |
    ForEach-Object { Join-Path $repo $_ } | Where-Object { Test-Path $_ }

$placeholders = Select-String -Path $docs -Pattern 'TODO_[A-Z_]+|<PLACEHOLDER'
if ($placeholders -and -not $TestBuild) {
    Write-Host ''
    $placeholders | ForEach-Object { Write-Host "  $($_.Path):$($_.LineNumber): $($_.Line.Trim())" }
    throw "Placeholders remain in the documentation. Fill them in, or this ships with them."
}

# --- the ini that goes in the box ----------------------------------------
# THE .example FILE IS THE SHIPPING CONFIG, and the refusal that used to live
# here is gone with the reason for it.
#
# It existed because the example was a development leftover: 79 live directives,
# eight keys that did not exist in ApplyValue at all, three binds to actions that
# did not exist, values outside their own ranges. Shipping it silently would have
# been shipping nonsense, so the packager made you say -AllowExampleIni.
#
# It was regenerated on 2026-09-09 from kSettings[] and the ini-only default
# table: 75 keys, one line of description each, EVERY LINE COMMENTED OUT. It
# changes nothing when installed -- it documents the built-in defaults, which are
# now the wearer's own tested values. registry-crosscheck.sh proves every key it
# names exists. There is nothing left to refuse.

# --- stage ----------------------------------------------------------------
$name = if ($TestBuild) { "titanfall2vr-$tag-TEST" } else { "titanfall2vr-$tag" }
$staging = Join-Path ([System.IO.Path]::GetTempPath()) "tf2vr-package-$([guid]::NewGuid().ToString('N'))"
$root = Join-Path $staging $name
$licenses = Join-Path $root 'licenses'
New-Item -ItemType Directory -Force -Path $licenses | Out-Null

Copy-Item $Dll (Join-Path $root 'titanfall2vr.dll')
Copy-Item $Ini (Join-Path $root 'titanfall2vr.ini')

# THE HAND-WRITTEN HASHES MUST MATCH THE DLL BEING PACKAGED.
#
# README-FIRST.txt is generated from a template, so its hash cannot drift. The
# ones in README.md and INSTALL.md are typed in by hand, and nothing checked
# them: any rebuild changes the DLL and leaves both documents quietly claiming a
# hash that no shipped file has. A reader who checks it -- which is the entire
# reason it is published -- would conclude the download was tampered with.
$docHash = (Get-FileHash -Algorithm SHA256 (Join-Path $root 'titanfall2vr.dll')).Hash.ToLower()
$hashDocs = @('README.md', 'INSTALL.md')
$stale = @()
foreach ($doc in $hashDocs) {
    $docPath = Join-Path $repo $doc
    if (-not (Test-Path $docPath)) { continue }
    $text = Get-Content $docPath -Raw
    if ($text -notmatch [regex]::Escape($docHash)) { $stale += $doc }
}
if ($stale.Count -gt 0) {
    Write-Host ""
    Write-Host "  the DLL being packaged is  $docHash"
    foreach ($doc in $stale) { Write-Host "  $doc does not contain that hash" }
    throw "Documented SHA-256 does not match the DLL being packaged. Update it, or the release ships a hash that fails the check it exists for."
}

# README-FIRST, generated so its version can never drift from the zip's.
$dllHash = (Get-FileHash -Algorithm SHA256 (Join-Path $root 'titanfall2vr.dll')).Hash.ToLower()
$template = Join-Path $PSScriptRoot 'package\README-FIRST.txt'
if (-not (Test-Path $template)) { throw "Missing $template" }
(Get-Content $template -Raw).
    Replace('@TF2VR_VERSION_TAG@', $tag).
    Replace('@TF2VR_DLL_SHA256@', $dllHash) |
    Set-Content -Path (Join-Path $root 'README-FIRST.txt') -Encoding UTF8

# --- licences -------------------------------------------------------------
# Ours, plus every dependency's own text. Dear ImGui is vendored, so its licence
# is in the tree; the OpenXR loader is fetched, so its licence comes out of the
# build tree and the package refuses to be built without it.
Copy-Item (Join-Path $repo 'LICENSE') (Join-Path $licenses 'titanfall2vr-LICENSE.txt')
Copy-Item (Join-Path $repo 'THIRD_PARTY_NOTICES.md') (Join-Path $licenses 'THIRD_PARTY_NOTICES.md')

$imguiLicense = Join-Path $repo 'plugin\third_party\imgui\LICENSE.txt'
if (-not (Test-Path $imguiLicense)) { throw "Missing $imguiLicense -- Dear ImGui is vendored and its licence must ship." }
Copy-Item $imguiLicense (Join-Path $licenses 'dear-imgui-LICENSE.txt')

$openxrSrc = Join-Path $repo 'plugin\build\_deps\openxr_sdk-src'
$openxrLicense = Join-Path $openxrSrc 'LICENSE'
if (-not (Test-Path $openxrLicense)) {
    throw @"
Missing $openxrLicense.

The OpenXR loader is Apache-2.0 and is linked into the DLL, so its licence text
must ship with the binary. It arrives with the FetchContent configure step --
run tools\build.ps1 before packaging.
"@
}
Copy-Item $openxrLicense (Join-Path $licenses 'openxr-sdk-LICENSE.txt')
$openxrCopying = Join-Path $openxrSrc 'COPYING.adoc'
if (Test-Path $openxrCopying) { Copy-Item $openxrCopying (Join-Path $licenses 'openxr-sdk-COPYING.adoc') }

# --- nothing personal in the box -----------------------------------------
$forbidden = Get-ChildItem -Path $root -Recurse -File |
    Where-Object { $_.Name -match '\.(pdb|log|bak.*|bmp|cache)$' -or $_.Name -like '*.headset.cache' }
if ($forbidden) {
    $forbidden | ForEach-Object { Write-Host "  unexpected: $($_.FullName)" }
    throw "The staging directory contains files that must never ship."
}

# --- zip + sidecar --------------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$zip = Join-Path $OutputDirectory "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $root -DestinationPath $zip -CompressionLevel Optimal

$zipHash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
Set-Content -Path "$zip.sha256" -Value "$zipHash  $name.zip" -Encoding ASCII

Remove-Item $staging -Recurse -Force

# --- the inventory, printed, because the checklist asks you to read it ----
Write-Host ''
Write-Host "Packaged $zip"
Write-Host ''
Write-Host 'Contents:'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
try {
    $archive.Entries | Sort-Object FullName | ForEach-Object {
        Write-Host ("  {0,-52} {1,10}" -f $_.FullName, $_.Length)
    }
} finally { $archive.Dispose() }

Write-Host ''
Write-Host "  DLL sha256  $dllHash"
Write-Host "  zip sha256  $zipHash"
Write-Host ''
Write-Host 'Put the DLL hash in INSTALL.md and the release notes.'
if ($TestBuild) {
    Write-Host ''
    Write-Host 'TEST BUILD. The documentation placeholder gate was skipped and the'
    Write-Host 'archive name says TEST. Do not publish this.'
}
