#pragma once

// ---------------------------------------------------------------------------
// WHAT THE ENGINE SAYS ABOUT ADS ZOOM. One owner, read live, no table.
//
// The wearer's instruction, 2026-08-26: *"the last thing I want is us keeping
// our own table of zoom levels for each weapon, when this should all be
// determined from the engine."* This is that, and it replaces four flat runs
// spent observing the rendered frustum through the D3D upload path.
//
//   GetFOV(player)        client.dll+0x2C5D00   .pdata root, 132 bytes
//   GetZoomFrac(player)   client.dll+0x2C8440   .pdata root, 202 bytes
//   weapon + 0x1364       float, the weapon's own declared zoom fov
//
// MEASURED AT THE RANGE, eight weapons, applied fov at frac = 1.00 against a
// 109.0 rest:
//
//   semipistol/autopistol 108.5  1.01x   alternator_smg 69.8/54.2  2.01/2.74x
//   wingman                85.2  1.52x   rspn101              54.2       2.74x
//   shotgun                77.5  1.75x   dmr                  46.5       3.26x
//                                        lmg          46.5/31.0  3.26/5.06x
//
// The rifle's 2.74x reproduces `KNOWN-GOOD-2026-08-23.md`'s rendered-frustum
// measurement of 2.74x exactly, by a completely independent route. That is the
// cross-check that makes this trustworthy rather than merely available.
//
// WHY IT IS ITS OWN MODULE. C1 needs the magnification on every camera upload
// and whether or not any probe is armed, so it cannot live inside a diagnostic.
// One tick a frame, three reads, and every consumer sees the same value.
// ---------------------------------------------------------------------------

// Once per plugin frame. Two engine calls and one field read; nothing at all
// until client.dll is loaded and a player exists.
void TickAdsZoom();

struct AdsZoomState {
    bool valid = false;
    float fovRest = 0.0f;       // GetFOV latched while frac == 0. NOT inferred.
    float fovNow = 0.0f;        // GetFOV: current, blend included
    float fovDeclared = 0.0f;   // weapon+0x1364, BEFORE cl_fovScale
    float frac = 0.0f;          // 0 hip, 1 fully aimed
    // tan(rest/2) / tan(now/2). 1.0 at rest by construction, so "am I zoomed"
    // needs no threshold, no latch and no running maximum.
    float magnification = 1.0f;
    // The same ratio against the weapon's own declared fov, scaled to applied
    // by cl_fovScale. This is what says whether the weapon is a MAGNIFIED OPTIC
    // rather than an iron sight, per weapon, from the weapon -- and it is a
    // property of the gun, so it holds while the blend is still ramping.
    float magnificationAtFull = 1.0f;
};

// False until a player, a weapon and a resting fov have all been seen.
bool GetAdsZoom(AdsZoomState* out);

// cl_fovScale, DERIVED rather than configured: applied fov / declared fov,
// measured at 1.549..1.551 across six weapons. Latched the first time the blend
// reaches full so it is read from the engine's own behaviour and not from a
// setting that could drift out of step with it. 0 until then.
float AdsZoomFovScale();
