# Building titanfall2vr

You do not need to build anything to play — see [INSTALL.md](INSTALL.md). This
is for changing the mod.

## Requirements

| | |
|---|---|
| Windows | 10 or 11, x64 |
| Compiler | MSVC v143 (Visual Studio 2022, or the 2022 Build Tools) with the **Desktop development with C++** workload |
| CMake | 3.24 or newer. The Visual Studio installer ships one; see below. |
| Git | needed at configure time — the OpenXR loader is fetched from GitHub |
| Standard | C++20 |

The build fetches the **OpenXR SDK loader** at the pinned tag `release-1.1.62`,
so the first configure needs network access. **Dear ImGui is vendored in-tree**
and is not fetched.

### CMake is not necessarily on PATH

The Visual Studio Build Tools install their own copy and do not add it to PATH.
On a default Build Tools install it is at:

```
C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
```

`tools\build.ps1` finds it for you. If you invoke CMake yourself and get
"cmake is not recognized", that path is why.

## Build

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

Or with CMake directly, using the preset. The presets live next to the
`CMakeLists.txt`, so run these from `plugin\`:

```powershell
cmake --preset release
cmake --build --preset release
```

Both produce `plugin\build\Release\titanfall2vr.dll`.

`build.ps1` also takes `-Debug` (build the Debug configuration too — it is never
deployed, it just keeps the compiler checking the tree) and `-Fresh` (discard
the CMake cache and reconfigure).

### The version is declared in one place

`TF2VR_VERSION_TAG` at the top of `plugin/CMakeLists.txt`. CMake generates
`version.h` from it, the DLL prints it in the log's first line, and
`tools\package.ps1` reads the same line for the zip name — so the archive, the
banner and the git tag cannot disagree. Bump it there and nowhere else.

**Release configuration only.** Debug links, and is kept linking so the compiler
keeps checking the whole tree, but a Debug DLL is never deployed and never
tested. Every measurement in this repository is a Release measurement.

## Install what you built

```powershell
powershell -ExecutionPolicy Bypass -File tools\install.ps1
```

It copies the DLL into your Northstar `plugins` directory and prints the SHA-256
of the source and the destination so you can see they match.

**Build and deploy are two steps, and the game must be closed for the second
one.** Windows holds the DLL open while the game is running, so a copy over a
live game leaves the old build in place — and a test then reports results for
code that was never loaded. `install.ps1` refuses to copy while the game is
running rather than failing quietly.

**A clean build produces a new hash.** That is expected: the hash identifies a
build, not a version. Record the hash of the DLL you actually tested.

To remove it again:

```powershell
powershell -ExecutionPolicy Bypass -File tools\uninstall.ps1
```

It deletes only what this mod installed, names each file first, and keeps your
`titanfall2vr.ini` unless you pass `-IncludeConfig`.

## Running without a headset

Set `profile = 1` in `titanfall2vr.ini` for the flat harness — the plugin comes
up, the hooks install, and everything that does not need a headset runs on the
monitor. This is the cheapest way to check that a change builds, arms and does
not crash before anyone puts a headset on.

`profile = 1` is the flat gate. `render.width = 0` is **not** — 0 means
"derive from the headset".

## Before you commit

Two gates, both scripts in `tools\`:

```bash
tools/registry-crosscheck.sh
```

**"Registry" here means this project's settings table, not the Windows
registry.** The plugin never reads or writes the Windows registry — there is not
one `RegOpenKey` in the tree. Settings live in `titanfall2vr.ini`, a plain text
file next to the DLL. The name is historical, from `settings_registry.h`.

Cross-checks the tables that have to cover each other exactly: the settings
table (`kSettings[]`), the `ApplyValue` key chain, the menu presentation table,
and the ini-only defaults table.
A settings row with no `ApplyValue` key is a control that silently does nothing;
a presentation row with no settings row is a label with no setting.

**Read the counts, not the word PASS.** It once passed with all three lists
empty after a file move left its paths stale. It now refuses a zero count, but
the habit is the real protection.

```bash
tools/scrub.sh
```

A content gate over the working tree and the commit message. It is installed as
a `pre-commit` and `commit-msg` hook; if you clone fresh, install the hooks
before your first commit. Its wordlist lives **outside** the repository and is
not distributed.

## Log

The plugin writes to `titanfall2vr.log` in `%LOCALAPPDATA%\titanfall2vr\`. It opens
with a banner naming the version, the build and the headset the runtime
reported, then the configuration it loaded and a content hash of it — so a
question about which build and which settings produced a result is answered by
the log rather than by memory.

`tools/logdig` digests it. The raw log runs to thousands of lines, most of it a
handful of repeated per-frame families; `logdig` prints the identity, the rare
lines, and the families as counts.

## Repository layout

```
plugin/CMakeLists.txt      the build
plugin/src/                12 subsystem directories
plugin/third_party/imgui/  vendored, pinned
plugin/*.asm               MASM interceptors
tools/                     build, install, package, scrub, crosscheck, logdig, pescan
docs/images/               the controller diagrams CONTROLS.md embeds
```

`plugin/src/` is split by subsystem: `ads`, `camera`, `core`, `diag`, `engine`,
`hands`, `host`, `hud`, `input`, `render`, `ui`, `xr`.

**Start at `plugin/src/host/host.h`.** Everything this mod does is behind three
calls — `HostOnInit`, `HostOnModuleLoaded`, `HostOnMainThreadFrame`. The
Northstar plugin class in `plugin/src/host/plugin.cpp` is a thin adapter over
them: three one-line forwards. If you are adding a second host, or just want to
know where a frame begins, that header is the map.

Some modules host live mechanisms behind names that read like diagnostics.
**Classify a module by the symbols it defines, not by its filename** — four of
them were nearly deleted on the strength of their names alone.

## Contributing

There is no formal process for the alpha. Open an issue first for anything
larger than a fix; the mod is a set of hooks into a shipping game binary, and a
change that looks local often is not.
