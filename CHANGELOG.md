# Changelog

All notable changes to titanfall2vr. Versions follow
[semantic versioning](https://semver.org/); the alpha is `0.y.z` and nothing
about the configuration format is stable yet.

## Unreleased

_No changes yet._

## v0.1.0

First public release. Titanfall 2's campaign in VR, as a native Northstar client
plugin — no Northstar script mod, no game files modified.

### Added

- **Alternate-frame stereo.** Each game frame renders one eye; the eyes take
  turns. Both are submitted together as an OpenXR projection layer, each with
  the head pose its own frame was rendered for.
- **6DoF head tracking**, with a configurable neck model, and an eye height that
  moves the camera, the weapon and the rounds together so aim stays true at any
  setting.
- **Motion-controller aim.** The weapon follows your hand, with a grip offset
  that can be calibrated live from the settings panel and saved to the ini.
- **Aiming down sights**, with a magnification cap, a scope passthrough
  threshold, an optional aim brace, and independent control over whether ADS
  turns and tilts your view to the gun.
- **VR controllers as a gamepad**, at the game's own input seam, so your
  in-game controller bindings apply unchanged. Snap and smooth turning, a turn
  response curve, separate turn and jump/crouch deadzones, stick separation,
  crouch as hold or either toggle, and haptics.
- **A d-pad modifier** for a Titan's four d-pad directions — held on the left
  thumbrest, or by raising the left controller beside your head on hardware with
  no thumbrest. The View button, its other use, also has a modifier-free route:
  hold Y.
- **An in-headset settings panel**, opened with both thumbstick clicks, holding
  69 live settings across eight categories, written back to the ini.
- **HUD placement for a headset**: the lower-left weapon and ammo group, the
  top-left cluster, name labels, the reticle pass, the waypoint marker and its
  distance text, and — in a Titan — the cockpit surround, the dash bars and the
  multi-target missile lock rings, each independently sized or placed.
- **A pilot body control**: weapon only, weapon and hands, or full body.
- **Resolution control** from the panel, applied on close.
- **The VR stack starts itself** when the game does. No keypress is required.

### Known issues

See [KNOWN-ISSUES.md](KNOWN-ISSUES.md), which has fifteen entries. The headline
items: the **first** launch with a given headset renders badly magnified for
about a minute before it corrects itself; the view has twice latched
at the wrong zoom for minutes at a time, unexplained; and the Virtual Desktop
runtime has crashed twice in a long day of testing, cause not yet attributed to
either side. There are also occasional frame spikes in play and at level load.

### Notes

- Single player only. Do not take this onto official servers.
- Tested on Play For Dream MR and Meta Quest 3, both through Virtual Desktop
  (VDXR). Other OpenXR runtimes are untested, not unsupported.
