#pragma once

#include <windows.h>

// ---------------------------------------------------------------------------
// THE HOST SEAM.
//
// Everything this mod does is behind three calls. A host -- anything that can
// load a DLL into the game and drive a frame -- implements those three and gets
// the whole plugin; it needs to know nothing else about it.
//
// Today the only host is Northstar, and `Titanfall2VrPlugin` in plugin.cpp is a
// thin adapter over these: its Init, OnLibraryLoaded and RunFrame are one line
// each. That is the entire point. The alpha ships on Northstar (PLAN-RELEASE
// section 6.2), and the standalone route (section 6.3) stays cheap to take
// because taking it means writing a second adapter over this header rather than
// unpicking the plugin from a framework it grew into.
//
// The three are the three moments that actually matter, and no more:
//
//   HostOnInit             once, as early as the host can manage. Registers
//                          the crash handler, reads the config, installs the
//                          module-load watches. Arms nothing.
//   HostOnModuleLoaded     each time a game DLL appears. Several hooks can only
//                          be installed in the window between a module existing
//                          and it running its own initialisation, and this is
//                          that window.
//   HostOnMainThreadFrame  once per frame, on the game's main thread. The whole
//                          per-frame body: the arm ladder, hotkeys, input, and
//                          every Tick and Advance.
//
// A host that cannot deliver HostOnModuleLoaded at the right moment can call it
// late; the hooks behind it retry and say so in the log. A host that cannot
// call HostOnMainThreadFrame on the main thread cannot use this plugin at all.
// ---------------------------------------------------------------------------

// `self` is this DLL's module handle: the config is loaded from beside it.
void HostOnInit(HMODULE self);

void HostOnModuleLoaded(HMODULE module, const char* name);

void HostOnMainThreadFrame();
