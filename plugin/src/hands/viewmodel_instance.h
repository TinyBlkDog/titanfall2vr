#pragma once

// ---------------------------------------------------------------------------
// ONE OWNER FOR "WHICH OBJECT IS THE LIVE C_BaseViewModel".
//
// The watchpoint (F1) and the placement pin (F2) both need it, and both need
// the SAME validity rule. Freed heap stays committed, so a readability test
// passes long after the entity is gone and a write lands in whatever was
// allocated there next -- viewmodel_bones.cpp learned that the hard way, and the
// only check that actually holds is re-reading the vtable pointer.
//
// Duplicating that rule in two files is how the two copies drift apart, and a
// drifted copy of a safety check is worse than no check, so it lives here.
// ---------------------------------------------------------------------------

// True while the object still reads as a C_BaseViewModel. Cheap: one guarded
// read and one compare, so callers may use it on every write.
bool ViewmodelInstanceStillValid(const unsigned char* instance);

// The live instance, walked back from the bone array the scan cached, or null.
// Does NOT run the scan -- the caller decides whether a 4.7 GB walk is
// warranted, because it is one and it is not free.
const unsigned char* FindLiveViewmodelInstance();

// WHICH ENTITY IS WHICH -- SETTLED BY THE BODYGROUP SWEEP, AND IT IS THE
// REVERSE OF WHAT THIS FILE ORIGINALLY SAID.
//
// C_BaseViewModel carries SIX switchable bodygroups and every one of them
// turned out to be a SCOPE OR A MUZZLE. Weapon attachments live on the weapon,
// so:
//
//     C_BaseViewModel             = THE GUN            (ptpov_*)
//     C_ViewmodelAttachmentModel  = THE ARMS AND BODY  (pov_pilot_*)
//
// Corroborated independently by the older bone-route note in viewmodel_bones.h --
// "driving the attachment 74 bones moved the entire BODY and the gun did not
// move at all" -- and by the shipped assets being authored apart.
//
// Everything F2 measured still holds with the labels swapped: the arms are
// PARENTED to the gun viewmodel, which is why moving the gun moves both, and
// why writing the arms own placement is silent (a derived child transform).
//
// Both classes carry the same placement fields, since +0x114 and +0x12C are
// C_BaseEntity members rather than viewmodel ones.
const unsigned char* FindLiveAttachmentInstance();
bool AttachmentInstanceStillValid(const unsigned char* instance);

// C_BaseViewModel placement fields, CORRECTED BY F1 from the store
// instructions in the commit at client.dll+0x3DB75B. The position was
// documented as +0x128 and is one float later than that; see
// F1-PLACEMENT-WRITER-FOUND.md.
constexpr unsigned kViewmodelAnglesOffset = 0x0114;    // pitch, yaw, roll
constexpr unsigned kViewmodelPositionOffset = 0x012C;  // x, y, z
// What the commit reads FROM. Kept because "is our write upstream or
// downstream of this" is the first question any future seam has to answer.
constexpr unsigned kViewmodelStagedPositionOffset = 0x0138;
constexpr unsigned kViewmodelStagedAnglesOffset = 0x0144;

// EVERY live instance of each class, not just the first.
//
// FH1 found EF_NODRAW already set on the attachment it resolved. That is what
// an inactive or pooled attachment looks like, and the resolve above returns
// whichever instance the handle table lists first, so which object we sampled
// was never established. These enumerate so the question can be settled by
// looking at all of them at once. Fills `out` with at most `max` pointers,
// each already vtable-validated, and returns how many.
int EnumerateAttachmentInstances(const unsigned char** out, int max);
int EnumerateViewmodelInstances(const unsigned char** out, int max);
