# Build titanfall2vr, Release.
#
# CMake is not necessarily on PATH: the Visual Studio Build Tools install their
# own copy and do not add it. This finds that copy, or any cmake already on
# PATH, and configures + builds the `release` preset.
#
# Build and deploy are TWO steps on purpose. This script never copies anything
# into a game installation -- tools\install.ps1 does that, and it refuses to
# run while the game is up.

[CmdletBinding()]
param(
    # Also build Debug. It is never deployed; it exists so the compiler keeps
    # checking the whole tree under a different set of assumptions.
    #
    # NOT named -Debug: [CmdletBinding()] already defines -Debug as a common
    # parameter, and declaring it again makes PowerShell refuse to run the
    # script at all -- "A parameter with the name 'Debug' was defined multiple
    # times". The script was unrunnable as shipped.
    [switch]$AlsoDebug,
    # Force a fresh configure, discarding the CMake cache.
    [switch]$Fresh
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$pluginDir = Join-Path $repo 'plugin'

function Find-CMake {
    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $roots = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022"
    ) | Where-Object { $_ -and (Test-Path $_) }

    foreach ($root in $roots) {
        $candidate = Get-ChildItem -Path $root -Filter 'cmake.exe' -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -like '*CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' } |
            Select-Object -First 1
        if ($candidate) { return $candidate.FullName }
    }
    return $null
}

$cmake = Find-CMake
if (-not $cmake) {
    throw @"
Could not find cmake.exe.

It is not on PATH, and no copy was found under a Visual Studio 2022 install.
Install the 'Desktop development with C++' workload (which brings CMake), or
put cmake 3.24+ on PATH, then run this again.
"@
}
Write-Host "cmake: $cmake"

$configureArgs = @('--preset', 'release')
if ($Fresh) { $configureArgs += '--fresh' }

Push-Location $pluginDir
try {
    & $cmake @configureArgs
    if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }

    & $cmake --build --preset release
    if ($LASTEXITCODE -ne 0) { throw "Release build failed ($LASTEXITCODE)" }

    if ($AlsoDebug) {
        & $cmake --build --preset debug
        if ($LASTEXITCODE -ne 0) { throw "Debug build failed ($LASTEXITCODE)" }
    }
} finally {
    Pop-Location
}

$dll = Join-Path $pluginDir 'build\Release\titanfall2vr.dll'
if (-not (Test-Path $dll)) { throw "Build reported success but $dll is not there." }

$hash = (Get-FileHash -Algorithm SHA256 $dll).Hash.ToLower()
$written = (Get-Item $dll).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')

Write-Host ''
Write-Host 'Built:'
Write-Host "  $dll"
Write-Host "  sha256  $hash"
Write-Host "  written $written"
Write-Host ''
Write-Host 'Nothing has been deployed. Close the game, then run tools\install.ps1.'
