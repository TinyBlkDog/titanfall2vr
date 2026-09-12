#pragma once

// ---------------------------------------------------------------------------
// N2 -- C-ENGINE. `docs/PLAN-CAMERA-2026-08-31.md`, rung N2.
//
// THE ONE THING SIX ELIMINATED MECHANISMS HAVE IN COMMON IS THAT THEY ARE ALL
// D3D-SIDE. Five patch the CONTENTS the engine uploads and one rebinds a slot;
// not one writes the ENGINE'S OWN CAMERA and lets the engine bake the eye.
// That is what every released VR mod in `C:\dev\othermods` actually does, it is
// the replan's preferred B1, and it has never been tried here.
//
// THE SOURCE, READ OFF THE BINARY RATHER THAN GUESSED. The scene-draw call at
// client+0x35AAE3 passes `this+0x12ECC0` as arg2. That block is not a bag of
// state: `client+0x359250` builds it, and the build is legible end to end.
//
//     0x359250(src, blk)                         the view build
//       0x636420(blk+0x40, src+0xE4, src+0xF0)   view matrix <- origin, angles
//       0x6343B0(blk+0x80, fov, aspect, ...)     projection
//       0x635370(blk+0x80, blk+0x40, blk+0xC0)   viewproj = proj * view
//       0x358690(blk)                            origin/basis <- view matrix
//
// So the block's layout is DERIVED, not assumed:
//
//     +0x00  camera origin, world space   (0x358690 writes it)
//     +0x0C  -1.0f
//     +0x10  basis rows, sign-masked      (0x358690 writes them)
//     +0x40  VIEW matrix, row-major 4x4, translation in COLUMN 3
//     +0x80  PROJECTION matrix
//     +0xC0  VIEWPROJ = proj * view
//     +0x180 fov          +0x190..0x19C viewport x,y,w,h
//     +0x1A0 copy of +0x00                +0x1B8 non-zero on the custom-matrix path
//
// ROW 0 OF THE VIEW MATRIX IS THE CAMERA RIGHT AXIS, AND IT IS UNIT LENGTH.
// That is decoded from 0x636420, whose first three stores are
//     [out+0] = sy*cr - sp*sr*cy
//     [out+4] = -sr*sp*sy - cr*cy
//     [out+8] = -sr*cp
// which is Source's `right` vector exactly. This is NOT the shader's
// c_cameraRelativeToClip row 0, which carries the projection's horizontal scale
// and cost this project a run when it was assumed to be unit; the two are
// different matrices and this one is built from sines and cosines alone.
//
// THE ORIGIN AND THE MATRICES LIVE IN DIFFERENT BLOCKS, AND THE FIRST RUN OF
// THIS RUNG PROVED IT THE HARD WAY. The scene draw takes its matrices from its
// arg2 block and its ORIGIN from this+0xA13C0 -- client+0x3725F1 copies that
// triple straight out -- and it never reads arg2 +0x00..+0x08 at all. Moving one
// and not the other does not move a camera-relative camera, it tears it in half:
// the private eye came back BLACK, with only the Titan outline and the crosshair
// surviving, while the uploaded camera and the write residual both said the
// write had worked perfectly. BOTH blocks are written now, and a gate refuses
// the write unless they hold the same camera to begin with.
//
// THEREFORE THE WHOLE EYE OFFSET IS ONE FLOAT PER BLOCK. For a view matrix V = R*T(-eye)
// with unit rows, moving the eye to eye + s*right changes exactly one element:
//
//     blk[+0x4C] -= s          (row 0, column 3, i.e. -dot(right, eye))
//
// and nothing else, because R*(s*right) = (s, 0, 0) when R's rows are
// orthonormal. No trigonometry, no hand-rolled matrix, no angle convention to
// get wrong. The two derived blocks are then rebuilt BY THE ENGINE'S OWN
// BUILDERS -- 0x635370 for the viewproj and 0x358690 for the origin and basis.
//
// AND THAT GIVES THE WRITE ITS OWN FALSIFIER, BEFORE ANY GPU WORK. 0x358690
// recovers the camera origin from the view matrix by inverting the rotation, so
// after the write the engine itself reports where it thinks the camera is. If
// blk+0x00 does not come back as (old origin + s * right) to within a hair, the
// write did not do what this comment says and the run stops there rather than
// being judged on a picture.
//
// THE RESTORE IS DEFERRED ON PURPOSE, and this is a deliberate deviation from
// the plan's "reverted immediately after". F1's own thread-attribution line
// says the scene draw issues no D3D on its own thread -- it queues -- so a
// restore placed immediately after the nested call can revert the block BEFORE
// whatever consumes it has read it. That failure is inert and silent, and it is
// indistinguishable from "the mechanism does not work", which is the exact
// false null that has already been mistaken for a refutation twice here.
//
// So the restore happens at the TOP of the next scene-draw hook entry, before
// pass 1, and only if the block still holds the bytes we left in it. The engine
// rebuilds the block from its source every frame (0x35A500 -> 0x359250, at
// client+0x35A9BB, which runs BEFORE our hook), so the expected outcome is that
// the restore finds the block already rebuilt and declines -- and the decline
// is counted, so "the engine restored it for us" and "our restore ran" are
// different numbers in the log rather than the same silence.
// ---------------------------------------------------------------------------

// `stereo.engine_ipd`, world units. The distance batch 2's eye is moved along
// the camera right axis. 0 disables the write; the rung's existence test uses
// 120, which is absurd on purpose -- a null at 12 units has twice been mistaken
// for a broken mechanism.
void SetEngineIpd(float units);
float EngineIpd();

// THE MAGNITUDE LADDER, on a key. One press steps to the next rung -- full,
// a fifth, a twentieth, then zero -- so one session photographs several
// magnitudes of the SAME lever in the SAME scene, and the last rung is the
// positive control. Two runs have now moved the camera correctly and come back
// blank while still ISSUING every draw, so the magnitude is the variable worth
// a run before any new mechanism is.
void ToggleEngineCameraWrite();
// The rung currently selected, in world units. 0 on the control rung.
float EngineIpdActive();
bool EngineCameraWriteArmed();

// Called from the scene-draw hook on the seam thread, between pass 1 and the
// nested pass. Returns true when the block was actually written.
// `scale` multiplies ActiveIpd() for THIS write. The write is RELATIVE
// (blk[+0x4C] -= s), so calls accumulate within a frame -- which is what makes
// the symmetric pair possible without a second mechanism: -0.5 before pass 1
// puts batch 1 at -half, then +1.0 before pass 2 carries it to +half.
bool WriteBatch2Camera(void* self, void* viewBlock, float scale = 1.0f);

// SYMMETRY. With only batch 2 moved, the pair is correct in SEPARATION but its
// midpoint sits half an IPD to one side of the head -- which the wearer sees as
// BOTH eyes sliding sideways the moment a burst arms. Armed, the pair straddles
// the head instead. Experiment slot C this build.
void ToggleSymmetricEyePair();
bool SymmetricEyePairArmed();

// Called from the scene-draw hook at entry, before pass 1. Restores the block
// if and only if it still holds exactly what the write left there.
void RestoreBatch2CameraIfUntouched();

// Read-only, once per burst: the three view blocks the engine finalises before
// the scene draw (this+0x12ECC0, +0xA13C0, +0xB55C0), with origin, viewport and
// fov. THE SKYBOX CHECK, and deliberately not a copy of the shipped one: the
// shipped guard identifies the sky camera from UPLOADS by origin distance and
// depends on g_referenceOrigin, which a flat run may never populate. This
// identifies it from the blocks themselves, which exist on every run.
void CensusViewBlocks(void* self);

// One status line per burst. Names every counter and every decline.
void ReportEngineCameraWrite();

// Per plugin frame. Resolves and PROLOGUE-VERIFIES the two engine builders once
// client.dll is up, so "the bytes at client+0x635370 are not what this rung was
// written against" reaches the log seconds after launch rather than in the
// middle of a run that has already been handed to the wearer.
void TickEngineCameraWrite();
