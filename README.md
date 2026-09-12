# Titanfall 2 VR

A VR mod for **Titanfall 2**'s single-player campaign. It is a Northstar client
plugin — one DLL and one ini — that hooks the game's DirectX 11 renderer and its
camera and drives them from an OpenXR session. No game files are modified and no
game assets are distributed.

Version **`v0.1.1`**. This is an alpha. Single player only; see
[Multiplayer](#multiplayer).

Developed and tested on a Meta Quest 3 and a Play For Dream MR, both through
Virtual Desktop's VDXR runtime. Other headsets and runtimes are untested, not
blocked.

> ### It ships at 75% resolution, not 100%
>
> Nothing in the mod measures your GPU, so the default is set for a card that
> may not be a fast one. **If the image looks soft, that is the reason, and it
> is one setting away.** Open the settings panel with both thumbstick clicks and
> raise **Resolution**. 100% is your headset's own full panel; the range goes to
> 200%.
>
> Raise it until the frame rate stops holding, then come back a step. See
> [Performance](#performance).

## Features

- Stereo rendering. One eye is drawn per game frame, alternating, each eye using
  the head pose from the frame it was drawn in. Each eye refreshes at half the
  headset's frame rate.
- 6DoF head tracking, with an adjustable neck model.
- Motion-controller weapon aim. The grip offset can be calibrated in the panel
  and is saved automatically.
- Adjustable eye height. View, weapon and shot origin move together.
- Aim-down-sights controls: a cap on view zoom that still allows scoped weapons
  to magnify, an optional steadiness aid, and separate control of whether ADS
  rotates the view to the weapon.
- Controllers are presented to the game as a gamepad, so in-game controller
  bindings apply. Snap and smooth turning, a turn response curve, separate
  deadzones for turning and for the right stick's vertical axis, three crouch
  modes, haptics.
- HUD elements repositioned and scaled individually: ammo, the top-left cluster,
  name labels, reticle, waypoints, and in a Titan the cockpit surround, dash
  bars and missile lock rings.
- In-headset settings panel. 72 settings in 8 categories, saved on change.
- Starts automatically with the game. No keypress required.

## Requirements

- Titanfall 2, on Steam or the EA app. The game binaries are identical on both.
- [Northstar](https://northstar.tf), installed and launching the game.
- An OpenXR runtime. Developed against Virtual Desktop's VDXR.
- A PC-VR-capable GPU. See [Performance](#performance).

## Install

1. Install Northstar and confirm the game launches through it.
2. Close the game.
3. Copy `titanfall2vr.dll` and `titanfall2vr.ini` from the release zip into
   `Titanfall2\R2Northstar\plugins`.
4. Set the OpenXR runtime to VDXR: Virtual Desktop, Streaming, OpenXR runtime.
5. Launch through Northstar and start the campaign.

No further configuration is required. [INSTALL.md](INSTALL.md) covers
troubleshooting if VR does not start.

`v0.1.1`, `titanfall2vr.dll` — SHA-256
`d8b72baacdeef507dd70934c11dad1740af4ab30d8a2fc8f4a616eee245ceb71`

## Settings panel

Click both thumbsticks together to open it.

| | |
|---|---|
| Either stick | Move between rows |
| A | Change the setting |
| B | Back |
| Grips | Change category |
| Both sticks | Close |

Changes are saved when made. Recentring is handled by the headset's own
recentre. [CONTROLS.md](CONTROLS.md) has the full controller map and every
setting.

## Performance

The mod ships at 75% resolution rather than 100%. Nothing in it measures the
GPU, and full resolution is a significant load.

Adjust **Resolution** in the settings panel. Lower it if the frame rate is poor;
raise it if the image is soft and the frame rate is fine. The change applies
when the panel is closed. Field of view is unaffected.

When reporting a frame rate problem, state the Resolution value.

Test system: Ryzen 7 7800X3D, RTX 5090, 32 GB, Windows 11; Quest 3 and Play For
Dream MR over Virtual Desktop.

## Known issues

Full detail, with workarounds, in [KNOWN-ISSUES.md](KNOWN-ISSUES.md).

| Issue | Detail |
|---|---|
| The game can lock up and require closing from Task Manager | Observed a handful of times over several days of play. No known trigger. Progress since the last checkpoint is lost. [#10](KNOWN-ISSUES.md#10-the-game-can-freeze-and-have-to-be-killed-open) |
| Occasional frame spikes | A stall every 20–40 seconds, and longer stalls at level loads. Measured inside the OpenXR runtime, not in the mod. [#5](KNOWN-ISSUES.md#5-occasional-frame-spikes-open) |
| Cutscenes are locked to your head | The camera follows your head instead of staying with the scene, which can cause motion sickness. Keep your head still through them. [#15](KNOWN-ISSUES.md#15-cutscenes-are-locked-to-your-head-open) |
| Fades and overlays are drawn on your right hand | Fade-to-black, loading overlays and the damage border appear as a rectangle in front of the right controller rather than across the whole view. Cosmetic. [#11](KNOWN-ISSUES.md#11-fades-and-loading-overlays-sit-on-the-right-hand-not-on-the-whole-view-open) |
| The loading screen shrinks to a small window before it clears | You briefly see the level behind it. Wait for the load to finish and press A as prompted. [#16](KNOWN-ISSUES.md#16-the-loading-screen-shrinks-to-a-small-window-just-before-it-clears-open) |

## In development

| | |
|---|---|
| Both eyes rendered in the same game frame | Re-entering the scene draw works, then hangs the engine after a few minutes. Compiled into this release and disabled; do not enable it. |
| The lock-up | Under investigation. |
| Wider headset and runtime coverage | Requires testers. |

## Multiplayer

**Do not use this on official servers.** The aim path writes a correctly
checksummed user command that a server cannot distinguish from a gamepad's,
which is the technique a cheat would use, and it is against the EULA.

On private Northstar servers, ask the owner. Relevant facts:

- Northstar has no anticheat and no client attestation. A server cannot detect a
  VR client, and cannot enforce a policy either way.
- No aim assistance is present. The steadiness aid moves the weapon a fraction
  of the hand's movement, in the same frame, with no target awareness. Only the
  player's own arms and body are hidden from rendering.
- Field of view is wider in VR, which is an advantage in a flat game.
- The eye-height correction is applied on both client and server. On a dedicated
  server the server half is not this mod, so the view would sit above the eye
  position the server uses for shots.

## Reporting bugs

[Open an issue](../../issues) with:

- the **version**, which the log prints at startup as
  `titanfall2vr v0.1.1 (<commit>) initialized`, with the build date and the
  DLL's own timestamp on the same line;
- headset, OpenXR runtime and GPU;
- the mission, and what was happening;
- steps to reproduce, if known;
- the first ~30 lines of `titanfall2vr.log`, from
  `%LOCALAPPDATA%\titanfall2vr\` — paste that into the address bar of a
  File Explorer window.
  They carry the build, the configuration and the headset banner.

The banner contains the runtime's own system name for the headset. That is
expected and safe to post.

## Build from source

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1
powershell -ExecutionPolicy Bypass -File tools\install.ps1
```

Requirements and the pre-commit gates are in [BUILDING.md](BUILDING.md).

## Credits

- [Northstar](https://github.com/R2Northstar) — the plugin host. The plugin
  interface headers here mirror its ABI.
- [bioshock-trilogy-vr](https://github.com/VR-Stereo-Hub/bioshock-trilogy-vr) —
  the in-headset overlay is adapted from its overlay module.
- [Dear ImGui](https://github.com/ocornut/imgui) — the settings panel.
- [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) — the runtime loader.
- [InjectableGenericCameraSystem](https://github.com/FransBouma/InjectableGenericCameraSystem)
  — its Titanfall 2 camera work identified the position structure.
- [Halo-MCC-VR](https://github.com/pancreations/Halo-MCC-VR),
  [CallOfDuty4_VR](https://github.com/jplakon/CallOfDuty4_VR) and
  [cyberpunk-vr-port](https://github.com/dariulone/cyberpunk-vr-port) — read
  while designing this mod. No code was copied.
- Titanfall 2 is the property of Respawn Entertainment and Electronic Arts. This
  project is unaffiliated with either.

Full attribution and licences: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Licensed MIT; see [LICENSE](LICENSE).
