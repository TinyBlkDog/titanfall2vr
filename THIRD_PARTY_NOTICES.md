# Third-party notices

titanfall2vr is MIT licensed (see [LICENSE](LICENSE)). It links, vendors and
adapts the work below.

## Vendored in-tree

| Component | Version | License | Source | Used for |
|---|---|---|---|---|
| Dear ImGui | v1.92.8 (`8936b58`) | MIT | https://github.com/ocornut/imgui | the in-headset settings panel |

Vendored under `plugin/third_party/imgui/`, with its licence text at
`plugin/third_party/imgui/LICENSE.txt` and its pin, provenance and
"no local modifications" statement at `plugin/third_party/imgui/VERSION`.
`imgui_demo.cpp` is deliberately not vendored.

## Fetched and linked at build time

| Component | Version | License | Source | Used for |
|---|---|---|---|---|
| OpenXR SDK (loader) | `release-1.1.62` | Apache-2.0 | https://github.com/KhronosGroup/OpenXR-SDK | the OpenXR runtime client |

Fetched by CMake `FetchContent` at the pinned tag above and linked into the DLL.
Apache-2.0 requires the licence text to accompany any binary distribution, so it
ships in the release archive as `licenses/openxr-sdk-LICENSE.txt`, alongside the
project's own `COPYING.adoc` which sets out the per-file licensing of the SDK.

The §4 NOTICE obligation does not arise: the OpenXR-SDK repository at
`release-1.1.62` carries no `NOTICE` file, and §4(d) applies only where the
work distributes one. Checked, rather than assumed — the packaging script
requires the `LICENSE` and refuses to build an archive without it.

## Adapted code

- **NorthstarLauncher** (MIT, https://github.com/R2Northstar/NorthstarLauncher) —
  `plugin/src/host/northstar_plugin_abi.h` mirrors the `PluginId001` plugin
  interface struct layouts so that this plugin can be loaded by Northstar
  without building against Northstar's private targets. The header carries the
  attribution comment.
- **bioshock-trilogy-vr** (MIT, https://github.com/VR-Stereo-Hub/bioshock-trilogy-vr) —
  the ImGui overlay in `plugin/src/ui/menu_overlay.cpp` is adapted from that
  project's overlay module, and the Dear ImGui pin above is the same commit that
  project ships. The file carries the attribution comment.

## Reference code — not vendored, not linked

- **InjectableGenericCameraSystem** (MIT,
  https://github.com/FransBouma/InjectableGenericCameraSystem) — byte patterns
  and the "angles live at +0x0C of the position struct" claim for Titanfall 2
  were taken from its Titanfall 2 camera implementation and re-verified against
  the shipping binary here. Cited in `plugin/src/camera/camera_hook.cpp`.

## Studied for concepts only — no code copied

Read at source while designing this mod. Nothing from them is present in this
repository; verified by grepping the tree for their identifiers, whose only hits
are prose in comments and two log strings.

| Project | License |
|---|---|
| [Halo-MCC-VR](https://github.com/pancreations/Halo-MCC-VR) | MIT |
| [CallOfDuty4_VR](https://github.com/jplakon/CallOfDuty4_VR) | GPL (inherited from KisakCOD) |
| [cyberpunk-vr-port](https://github.com/dariulone/cyberpunk-vr-port) | MIT |

## Artwork — not ours, and NOT under this repository's licence

**Meta Quest Touch Plus controller line art** — © Meta Platforms, Inc.
Embedded in `docs/images/controls-left.svg` and `docs/images/controls-right.svg`.

From the `oculus-controller-art` pack's own attribution file:

> You may use these images solely for referring to the corresponding product in
> your video game or VR experience (including manuals for users). Otherwise, you
> may not use these images, or any Oculus trademarks, logos or other
> intellectual property, including but not limited to use on merchandise or
> other product such as clothing, hats, or mugs. Do not use the Oculus images in
> a way that implies a partnership, sponsorship or endorsement; or features
> Oculus on materials associated with pornography, illegal activities, or other
> materials that violate Oculus Terms.
>
> THE IMAGES ARE PROVIDED TO YOU ON AN "AS IS" BASIS AND YOU ARE SOLELY
> RESPONSIBLE FOR YOUR USE OF THE IMAGES. OCULUS DISCLAIMS ALL WARRANTIES
> REGARDING THE IMAGES, INCLUDING WARRANTIES OF NON-INFRINGEMENT. OCULUS SHALL
> NOT BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, CONSEQUENTIAL OR
> PUNITIVE DAMAGES ARISING FROM OR RELATED TO YOUR USE OF THE IMAGES.
>
> For the avoidance of doubt, this license shall not apply to the Oculus name,
> trademark or service mark, logo or design.

**This is why those two files are excluded from the MIT licence in `LICENSE`.**
That licence covers this project's own work and cannot be extended over
somebody else's artwork; a reader who assumed MIT applied to everything in the
tree would be wrong about those two files, so it is said here plainly.

The permitted use is the one being made of it: CONTROLS.md is a manual for
users, and the images are there to show which physical control does what.
Nothing in this project carries a Meta or Oculus name, wordmark, logo or badge,
and nothing here states or implies a partnership, sponsorship or endorsement.
The home button the artwork depicts is deliberately left unlabelled, because
this mod does not use it.

## The game

titanfall2vr contains no Titanfall 2 assets, no decompiled game code, and no
game binaries. It reads and hooks the game you already own at runtime. Titanfall
is a trademark of Respawn Entertainment and Electronic Arts; this project is not
affiliated with, endorsed by, or connected to either.
