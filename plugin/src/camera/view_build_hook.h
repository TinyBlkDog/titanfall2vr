#pragma once

// Passive trace of the upstream CViewRender view-build method. F4 captures a
// bounded call count; it never writes game camera or renderer state.
void EnsureClientViewBuildHookInstalled();
// Whether the detour is actually in place. The install refuses when the
// upstream bytes differ, and a bone write armed against a seam that was never
// installed would present as exactly the same silence as a dead write -- which
// is the class of null this whole step exists to abolish.
bool IsClientViewBuildHookInstalled();
void RemoveClientViewBuildHook();
void BeginClientViewBuildTrace();
void AdvanceClientViewBuildTrace();
void EnsureClientViewCallsiteHookInstalled();
void RemoveClientViewCallsiteHook();
void EnsureClientViewSlotPrepareHookInstalled();
void RemoveClientViewSlotPrepareHook();
