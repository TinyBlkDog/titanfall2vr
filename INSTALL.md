# Installing titanfall2vr

Five steps. It is one DLL and one ini into one folder.

## Before you start

You need:

- **Titanfall 2**, on Steam or the EA app. Both work and are identical: the game
  binaries hash the same on either store, so every address this mod uses holds
  on both.
- **[Northstar](https://northstar.tf)**, installed, and launching the game
  successfully at least once. titanfall2vr is a Northstar client plugin and does
  not run without it.
- **An OpenXR runtime.** This was developed and tested against **Virtual
  Desktop's VDXR**, on **Play For Dream MR** and **Meta Quest 3**. Anything else
  is untested rather than unsupported — if yours works, please say so.
- **A PC-VR-capable GPU.**

## 1. Get the release

Download `titanfall2vr-v0.1.0.zip` and its `.sha256` sidecar from the
release page.

| file | SHA-256 |
|---|---|
| `titanfall2vr.dll` | `821525578a576a156b35a7c886bad23a64fc7f8c522ae1b4d89cc04092a8400a` |

Check the DLL against that hash if you did not get the zip from the project's
own release page.

## 2. Close the game

Windows holds a loaded DLL open. Copying over a running game leaves the old file
in place and nothing tells you it happened.

## 3. Copy two files

From the zip, put **both** of these into your Northstar plugins directory:

```
titanfall2vr.dll
titanfall2vr.ini
```

The directory is inside your Titanfall 2 installation:

```
Titanfall2\R2Northstar\plugins
```

Create it if it is not there. Typical full paths:

```
C:\Program Files (x86)\Steam\steamapps\common\Titanfall2\R2Northstar\plugins
C:\Program Files\EA Games\Titanfall2\R2Northstar\plugins
```

**The ini changes nothing on a fresh install, and you should still copy it.**

Every line in the shipped ini is commented out. It is a reference: each of the
75 settings, with one line saying what it does. The values it documents are the
built-in defaults, and those are what the mod runs on whether the file is there
or not — this is the config the mod was developed and played on, not a starting
point you are expected to tune.

So the ini is the file you edit when you want to change something, and the file
you read when you want to know what there is to change. Copy it now and it is
there when you want it.

## 4. Point your headset at the game

Set your OpenXR runtime **before** launching. In Virtual Desktop: **Streaming →
OpenXR runtime → VDXR**. Connect, then launch the game from inside Virtual
Desktop.

## 5. Launch through Northstar and play

Start the campaign. The VR stack arms itself.

---

## Two things Windows and Steam will do

**Windows may flag an unsigned DLL** being injected into a game. That is exactly
what this is: an unsigned mod DLL loaded into Titanfall 2 by Northstar. If your
antivirus quarantines it, that is the judgement it is making. The published
SHA-256 is how you check you have the file the project built.

**Steam users: turn the Steam overlay off for Titanfall 2.** It injects into the
same process and hooks the same DirectX presentation path. It has not been
tested alongside this mod.

---

## Changing settings

Click **both thumbsticks at once** in game. Sixty settings, all live, saved back
to the ini. See [CONTROLS.md](CONTROLS.md).

To edit the ini by hand, open `titanfall2vr.ini` next to the DLL and restart
the game. Most of what is in there is on the settings panel as well, where a
change takes effect as you make it.

**Set your resolution once.** Changing it while playing costs you two ability
buttons until you restart the game; see
[KNOWN-ISSUES.md](KNOWN-ISSUES.md#1-changing-resolution-during-play-broke-the-ability-buttons-no-longer-observed).

---

## When it does not come up

Everything the mod does is in the log, which is `titanfall2vr.log` in your
`%LOCALAPPDATA%\titanfall2vr\` folder (paste that into the address bar of a file
window). Its first lines name the version, the build, the configuration it read
and the headset the runtime reported.

**Nothing at all in the log, or no log file.** The plugin was not loaded.
Northstar is either not running the game or not finding the DLL. Check the DLL
is in `R2Northstar\plugins` and not in a subfolder of it, and that Northstar
itself starts the game.

**The log exists but the game is flat.** Look for the OpenXR lines. If the
session never comes up, the runtime is the place to look: check VDXR is
*selected* as the OpenXR runtime, not merely installed, and that you launched
the game with the headset already connected.

**Stereo works but the controllers do nothing** — you cannot move the menu
cursor, and the weapon ignores your hands. The input path is not armed. `autoarm`
brings up *rendering* only and never touches input; these four lines are what
turn the controllers on. All four are already the built-in defaults, so a fresh
install has them without you doing anything.

**This one has to be set before the game starts.** The game asks once, during
startup, whether a gamepad exists, and attaches the slot only if the answer is
yes. There is no reconnect path, so a pad enabled after that moment is never
polled.

**It comes up but the weapon does not follow your hand.** The VR stack starts
itself, in stages, and the setting that controls how far it goes is `autoarm`.
It is **5 by default**, which is the whole stack, so this should not happen on a
fresh install. If you have edited the ini, check you have not lowered it: 4
stops one stage short and gives you stereo with a weapon that stays put.

**This build binds no keyboard key at all**, so there is nothing to press and
nothing to press by accident.

**The weapon sits wrong in your hand.** That is calibration, not a fault. Open
the settings panel with **L3 + R3**, go to **Hands & weapon**, and move **Grip
forward**, **Grip right** and **Grip up** while you hold the gun. They move it
live and save themselves back to the ini.

**It runs, but badly.** Lower the resolution before anything else. Open the
settings panel with **L3 + R3** and lower **Resolution**; it applies when you
close the panel. The mod ships at 75% because nothing in it measures your GPU,
and 75% is still a lot of pixels on a mid-range card.

If it runs well and you want it sharper, go the other way: `1.0` is your
headset's own full resolution.

Your field of view does not change at any setting.

**Something else.** [KNOWN-ISSUES.md](KNOWN-ISSUES.md) first, then open an issue
with the head of your log — [README.md](README.md#reporting-bugs) lists what to
include.

---

## Uninstalling

Delete `titanfall2vr.dll` from `R2Northstar\plugins`. The game reverts to normal
immediately; nothing else was changed. Keep or delete `titanfall2vr.ini` as you
like — it is only your settings.

If you built from source, `tools\uninstall.ps1` does the same and names what it
removes first.
