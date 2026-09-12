# Changelog

All notable changes to titanfall2vr. Versions follow
[semantic versioning](https://semver.org/); the alpha is `0.y.z` and nothing
about the configuration format is stable yet.

## Unreleased

_No changes yet._

## v0.1.1

Everything below came out of the first day of reports on v0.1.0.

### Added

- **Field of view** and **Field of view: auto**, in the View category. Both
  were reachable only by editing the ini. On one headset the automatic
  derivation asks for a view 45% wider than the display can show, which is
  5.5 megapixels a frame of world nobody can see -- so this is not a
  developer knob. Note that neither changes how big anything looks: the image
  is submitted with its own geometry, so a wider view shows more world at the
  same size, and costs more to draw.
- **World scale**, in the View category. The mod sizes the world from your
  own IPD, converted at one game unit per inch, and Titanfall is not built at
  exactly that scale -- so the world reads oversized and the 3D reads weak,
  and the narrower your IPD the worse it is. Lower it if hands and weapons
  look too big; the depth strengthens as you do, because both come from the
  same number. 1.00 is the old behaviour exactly.

### Changed

- **The automatic field-of-view derivation now ships OFF.** It produces a
  value with nothing bounding it against what the headset can display -- on
  the one headset measured it asked for a view 45% wider than the display --
  and it is the suspect in [issue 17](KNOWN-ISSUES.md), a view that comes up
  fully zoomed in. The typed default it falls back to was dialled on real
  hardware, and both controls are now in the panel if it is wrong for you.
- **A third fewer pixels, for an identical picture.** The render buffer was
  forced to a 1.6 aspect to stop the engine letterboxing the world pass. With
  the engine's own letterbox dial armed -- now the default -- the buffer
  follows the headset's lenses instead. Measured on a Quest 3 over Virtual
  Desktop: 5222x3264 to 3528x3264, 17.0 megapixels to 11.5, with the
  horizontal field of view landing on the headset's to within 0.01%. No
  visible difference; a third less work.

### Fixed

- **World scale actually does something.** It scaled the eye separation and
  the units-per-metre by the same factor, which cancel exactly: the game's
  geometry is fixed in units, so rescaling both renders the same image by
  definition. The separation in millimetres never moved at any value. Only
  hands and weapons appeared to respond, because their placement converts
  metres to units.
- **A failed headset-cache write is no longer silent.** If the plugins folder
  cannot be written -- the usual cause is the game sitting under Program
  Files -- the cache never lands, so every launch derives the resolution from
  scratch and looks badly magnified for the first minute, for ever. It said
  nothing at all. It now names the file, the error and the likely cause.
- **A failed settings save is no longer silent.** The panel's write to the
  ini returned a result nobody checked, so on the same machines every change
  reverted on the next launch with no explanation anywhere.

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
  72 live settings across eight categories, written back to the ini.
- **HUD placement for a headset**: the lower-left weapon and ammo group, the
  top-left cluster, name labels, the reticle pass, the waypoint marker and its
  distance text, and — in a Titan — the cockpit surround, the dash bars and the
  multi-target missile lock rings, each independently sized or placed.
- **A pilot body control**: weapon only, weapon and hands, or full body.
- **Resolution control** from the panel, applied on close.
- **The VR stack starts itself** when the game does. No keypress is required.

### Known issues

See [KNOWN-ISSUES.md](KNOWN-ISSUES.md), which has sixteen entries. The headline
items: the **first** launch with a given headset renders badly magnified for
about a minute before it corrects itself; the view has twice latched
at the wrong zoom for minutes at a time, unexplained; and the Virtual Desktop
runtime has crashed twice in a long day of testing, cause not yet attributed to
either side. There are also occasional frame spikes in play and at level load.

### Notes

- Single player only. Do not take this onto official servers.
- Tested on Play For Dream MR and Meta Quest 3, both through Virtual Desktop
  (VDXR). Other OpenXR runtimes are untested, not unsupported.
