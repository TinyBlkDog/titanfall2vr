#pragma once

// ---------------------------------------------------------------------------
// THE PLAYER'S OWN VIEW OFFSET, which is the thing that should have been moved
// all along.
//
// The height correction currently lives in camera_hook.asm, which writes the
// camera position through a pointer inside a RENDER-SIDE accessor. That moves
// the eye the frame is drawn from and nothing else -- so the round still leaves
// the game's own eye ten units lower and flies along the aim direction, and its
// path no longer lies on the wearer's sight line. Measured: impacts below the
// reticle. It is parallax, it varies with range, and no reticle nudge can
// cancel it.
//
// The engine derives the camera, the shot origin, the reticle and the weapon
// from ONE quantity: the player's eye = entity origin + view offset. Raise the
// view offset and all four move together, with nothing left to keep in sync.
// That is what every released mod ends up driving, and it is the difference
// between a fix and an offset bolted onto the end.
//
// FINDING IT WITHOUT A SIGNATURE. Titanfall 2 exposes no view height cvar and
// this project has no symbols, so the field is identified by ARITHMETIC IDENTITY
// rather than by a byte pattern:
//
//     entity origin .z  +  view offset .z  ==  the camera height we already read
//
// The view offset is additionally (~0, ~0, 40..90) -- a vector whose x and y are
// zero and whose z is a human eye height in Source units. Requiring both at once,
// against a camera height measured independently by the census, is a far
// stronger identification than either alone, and it re-verifies itself on every
// map rather than trusting an RVA that a patch can move.
// ---------------------------------------------------------------------------

// Once per game tick, with the camera height the census already reads. Does the
// identification the first time it can, then applies the raise. Safe to call
// before a level is up: it does nothing without a live player.
void PlayerEyeTick(float cameraZ);

// `vr.eye_write` -- default OFF. Arms the write to the player's view offset.
// A config key rather than a rebuild, so backing it out is one line if it
// misbehaves in a way a guarded read cannot catch.
void SetPlayerEyeWriteEnabled(bool enabled);

// True once the field is identified AND the write is armed -- i.e. the height is
// being applied at the player's eye rather than at the render camera. The camera
// offset stands down when this is true, or the two would both apply and the
// wearer would be raised twice.
bool PlayerEyeWriteActive();

// How much to raise the eye, in Source units. Shared with the camera-side path
// so there is one number, not two that can drift apart.
void SetPlayerEyeRaiseUnits(float units);

// THE SAME IDENTIFICATION, READ-ONLY. Runs the search if it has not run, then
// reports the origin z and the view offset z the game currently holds without
// writing either. The aim census uses it to state `eye = origin + offset` as a
// measured identity beside the camera height, which is the check that says the
// field is still the right one on this map rather than assuming it.
//
// False means the field is not identified or the player is not live, and the
// caller must print nothing rather than print zeroes.
bool PlayerEyeReadOnly(float cameraZ, float* originZ, float* viewOffsetZ, int* fieldOffset);

// The local player entity, for read-only inspection by the census.
//
// The census needs OUR OWN model's attachments, not only the faced one's: the
// faced character may simply be TALLER, and "the camera is 3.4 units below that
// model's HEADFOCUS" cannot tell a short player from a low camera. Reading the
// same two attachments off the player's own body settles it, because both are
// then measured from the same skeleton.
//
// Null when no player is live. Never write through this.
const void* PlayerEyeLocalPlayerForReading();

// The player's collision hull, found by shape and confirmed by the crouch.
// Read-only. See the note at the definition for why the attachment route could
// not answer this and why the crouch is the control.
void LogPlayerHullCandidates(float cameraZ);

// vr.eye_lift -- THE HEIGHT FIX, AT THE GAME'S OWN SOURCE. Default 0 = off.
//
// A ratio, not a distance, and one number for both stances. The measurement:
// the hull is 72.00 standing and 47.00 crouched (confirmed by the crouch moving
// it) against view heights of 60.00 and 38.00, so the eye sits at 83.3% of
// stature where a human eye is at 93.3%. 93.3/83.3 = 1.120 lifts it to a human
// eye line and scales itself with the stance, so crouching needs no second
// constant and no table.
//
// It is written into the player-settings record the ENGINE reads:
//
//   record = server.dll + 0x1351978 + settingsIndex*0x68D0 + stance*0x110
//   view height = [record + 0x08]        (stance 0 standing, 1 crouching)
//
// resolved from GetStandingViewHeight/GetCrouchingViewHeight, which differ only
// in the stance argument, through a lookup that is pure address arithmetic with
// no allocation and no sharing between classes.
//
// WHY HERE AND NOT AT A READER. Every consumer reads this one field -- the
// engine's own view offset, and therefore the camera, the gun, the bullets and
// the reticle. Correcting the source moves them together by construction, which
// is what the render-camera raise could not do (it moved the eye and left the
// shot origin) and what the per-tick view-offset write could not do (it fought
// prediction and vibrated the gun).
//
// IT REFUSES TO WRITE UNLESS IT RECOGNISES WHAT IT FOUND. The addresses must
// already hold plausible view heights AND the standing one must match the value
// the census reads independently out of the player entity at +0xAC. Arithmetic
// that lands somewhere unrecognised is a wrong address, not a fix.
void SetEyeLiftRatio(float ratio);
void ApplyEyeLift(float measuredStandingViewHeight);

// THE THREE-WAY DISCRIMINATOR for the settings route. Read-only.
//
// The server's context builder caches the per-stance record and reads the view
// height out of it, so the record IS consumed server-side -- yet writing it to
// 67.20 and then crouching gave 60.00 -> 38.00 -> 60.00 at the entity. Exactly
// three things can explain that, and they are told apart by printing the
// RECORD and the ENTITY on one line across a stance change:
//
//   record 67.20 while entity reads 60.00 -> the field is not the source, and
//                                            the settings route is dead
//   record back at 60.00                  -> our write was reverted, and the
//                                            fix is WHEN we write, not where
//   record 67.20 and entity follows it    -> it worked and the earlier run was
//                                            measuring something else
void LogViewHeightRecordVsEntity(float entityViewOffsetZ, float cameraZ);

// WHERE THE LERP ENDPOINTS LIVE. Read-only scan of the player entity.
//
// Three writes to the settings tables have now been verified landing and
// ignored: server and client both read 60.00, both took 67.20, and the entity
// went on lerping 60.00 <-> 38.00 regardless. So the endpoints are not being
// looked up per stance change -- the entity is holding them, cached at spawn.
//
// Which makes them findable the same way the view offset itself was: by value,
// in the entity, with the CROUCH as the control. The standing endpoint reads
// 60.00 and never moves; the live offset at +0xAC sweeps between them. An
// offset that holds 60.00 while +0xAC is at 38.00 is a cached endpoint and not
// a copy of the current value, and that distinction is the whole point of
// sampling this crouched.
void LogViewHeightEndpointCandidates(float entityViewOffsetZ, float cameraZ);
