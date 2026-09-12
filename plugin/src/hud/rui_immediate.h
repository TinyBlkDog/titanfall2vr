#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// THE SECOND RUI DRAW PATH, AND THE DESCRIPTOR POOL.
//
// Every HUD instrument this project owns is built on ONE seam: engine+0xFC500,
// which virtual-calls a widget through its descriptor's slot +0x68. The census,
// the classifier, the substitution table, the size control and the unzoom all
// hang off it.
//
// There is a SECOND path. engine+0xFC960 is the engine RUI interface's slot
// 0x28 (the table is built at engine+0x54B40; the client keeps a copy at
// client+0xC3DA00 and calls through it). It builds its own scratch block and
// calls the descriptor at slot **+0x70**, not +0x68. Nothing in this plugin has
// ever referenced that function, that table slot, or that descriptor offset.
//
// Those two are the WHOLE set: `pescan vcall engine.dll 68/70` over the RUI
// region (0xF9000..0xFD000) finds exactly three call sites, and one of them
// (engine+0xFB430) is an asset lookup on a different interface, not a widget
// draw. So a widget is drawn by FC500 or by FC960 and by nothing else.
//
// WHY THIS EXISTS. The missile-lock HUD is drawn by two ui(11) widgets that the
// census has never once seen across 146 archived logs, 74 of which recorded a
// census, while the wearer reports missiles were active in the last four or
// five runs. The widgets are named in the binary, each by a single reference:
//
//   ui(11)+0x7E190  rui/hud/smart_core/smart_ammo_bracket        the target bracket
//   ui(11)+0x1E600  smart_ammo_corner / _lock_image / _measure   the aim square
//
// So either they draw through the path nothing watches, or they do not draw at
// all and the brackets are not RUI. This module separates those two, and it
// does not depend on being right about which: the POOL SCAN reads the game's
// descriptor pool directly and answers "does a descriptor for this widget even
// exist" without any draw path being involved.
// ---------------------------------------------------------------------------

// `rui.immediate` in the ini. Read-only throughout: the slot swap forwards
// every call to the engine's own function and changes nothing about the draw.
void SetRuiImmediateWanted(bool wanted);
bool RuiImmediateWanted();

// Per plugin frame. Installs the slot claim once the modules are up, runs the
// pool scan once the game is rendering a world, and prints the status block on
// its own five-second clock. Every counter in that block prints whether it is
// zero or not: an uncounted decline reads exactly like a widget that never drew.
void AdvanceRuiImmediate(bool worldReady);

// F3. Stamps a mark in the log and dumps everything: a fresh pool scan, both
// path censuses, and the watchlist. The wearer presses it while the brackets
// are on screen, which labels the moment.
void RuiImmediateMark();

// Called by the FC500 census for each NEW identity it discovers, so this module
// can read that descriptor's +0x70 as well as its +0x68 and run the watchlist
// over both. The first descriptor handed over is also kept as the POSITIVE
// CONTROL for the pool scan: a scan that cannot find a descriptor the live
// census is holding in its hand is disqualified, loudly.
void RuiImmediateNoteFc500Descriptor(std::uintptr_t descriptor);
