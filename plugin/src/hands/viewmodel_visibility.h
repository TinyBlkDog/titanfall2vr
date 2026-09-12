#pragma once

// ---------------------------------------------------------------------------
// HIDE THE ARMS, KEEP THE GUN.
//
// The bodygroup sweep answered its question and the answer inverted an
// identification I had backwards. The six switchable groups on
// C_BaseViewModel turned out to be SCOPES AND MUZZLES, so:
//
//     C_BaseViewModel             = THE GUN            (ptpov_*)
//     C_ViewmodelAttachmentModel  = THE ARMS AND BODY  (pov_pilot_*)
//
// Independently corroborated by the older bone-route note in viewmodel_bones.h --
// "driving the attachment's 74 bones moved the entire BODY and the gun did not
// move at all". Everything F2 measured still holds with the labels swapped:
// the arms are parented to the gun's viewmodel, which is why moving the gun
// moves both and why writing the arms' own placement is silent.
//
// That is much better news than a merged mesh. The arms are a SEPARATE ENTITY,
// so hiding them is a per-entity operation rather than mesh surgery.
//
// THE LEVER, resolved offline from client.dll:
//
//     client.dll+0x3F0140   SetVisible(rcx = entity, edx = visible)
//
//     +0x28  cmp byte ptr [rcx+0x4B8], 0     <- the gate
//     +0x2F  jne +0x41
//     +0x31  lea rcx,[rip+...] ; jmp <error>  <- refusal when the gate is 0
//     +0x41  eax = 1 << visible
//     +0x4D  or  [entity+0x170], eax          request this state
//     +0x56  and [entity+0x16C], ~eax         clear the other
//     +0x64  jmp qword ptr [vtable+0x338]     apply
//
// Script_Hide is a two-instruction thunk into it: `xor edx,edx ; jmp`.
//
// THE GATE IS READABLE, so the refusal is a thing to CHECK rather than a thing
// to discover. This never calls with the gate clear. That matters: the handoff
// records Hide() refusing a networked entity, and the refusal here is an error
// path, not a return value.
// ---------------------------------------------------------------------------

// One keypress. Reports the gate byte on both entities first, then -- only for
// entities whose gate allows it -- hides the ARMS, restores them, hides the
// GUN as a control, and restores that too. Everything is put back.
void ToggleViewmodelVisibilityProbe();
bool IsViewmodelVisibilityProbeRunning();
void AdvanceViewmodelVisibilityProbe();
