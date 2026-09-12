// The marker widget's RUI virtual machine: read it, and pin what it misplaces.
//
// Both entry points are scoped to the ONE substituted marker widget
// (`rui.marker_target`) and applied in RuiA3AfterDraw -- after the widget's
// program has filled the register file and before the engine's packer reads
// it. See rui_asset_dump.cpp for the offline read and the run that named every
// register.
#pragma once

#include <cstdint>

// `rui.dump = 1`. READ-ONLY. Logs the layer, descriptor, program, every
// register in pixels, the args pool and the element and evaluator tables.
// Fires once for reference and then only when the defect is present, capped.
void SetRuiAssetDump(bool selected);

// `hud.marker_text_x`. THE FIX. The fraction across the screen the waypoint's
// distance text and leader line are pinned to; 0.48 is the game's own healthy
// value and 0 is off. The target icon is never moved, heights are never
// touched, and the leader line is re-attached so its far end stays on the
// target. F6 steps the ladder.
void SetRuiMarkerTextX(float pin);
float RuiMarkerTextX();
void StepRuiMarkerTextX();

// Called by RuiA3AfterDraw for the marker widget only.
void RuiMarkerAfterDrawInstrument(void* layer, std::uint8_t* scratch);

// `hud.widget_scale`. Scales every substituted type-3 HUD widget EXCEPT the waypoint
// marker about the SCREEN CENTRE -- sizes and positions together, a true
// similarity transform, so the top and bottom segments come in rather than
// merely shrinking in place. 1.0 is inert. The marker is excluded because its
// target is re-projected from a world point; it keeps `hud.marker_size`.
// Needs `hud.widget_arm = 1`, which is what substitutes the widgets.
void SetHudWidgetScale(float scale);
float HudWidgetScale();
// hud.ll_shift_x: moves the loadout block and earn meter ACROSS after the
// uniform scale, position lanes only, so the size is unchanged. Positive is
// right, as a fraction of screen width. F6 steps the ladder this build.
void SetHudLowerLeftShiftX(float dx);
float HudLowerLeftShiftX();
// The titan's larger loadout bar is a different object and wants its own
// amount. Applied only once the titan state has SETTLED and the player is in
// a titan; on foot the value above applies.
// Read-only census of every substituted RUI draw, upstream of the type mask,
// and the wearer's tag: what is on screen at the moment the key is pressed.
void RuiNoteWidgetDraw(unsigned rva, long typeBits, const std::uint8_t* scratch);
void RuiTagVisibleWidgets();
// Per-widget dials for the two the tag identified in the gauntlet.
// The three dialled widgets, as live settings rather than constants.
void  SetHudPromptScale(float v);        float HudPromptScale();
void  SetHudPromptShiftY(float v);       float HudPromptShiftY();
void  SetHudInstructionScale(float v);   float HudInstructionScale();
void  SetHudInstructionShiftY(float v);  float HudInstructionShiftY();
void  SetHudTimerScale(float v);         float HudTimerScale();
void  SetHudTimerShiftX(float v);        float HudTimerShiftX();
void  SetHudTimerShiftY(float v);        float HudTimerShiftY();
void  SetHudHighlightScale(float v);     float HudHighlightScale();
void StepInstructionPanelScale();
void StepGauntletTimerShift();
void SetHudLowerLeftShiftXTitan(float dx);
float HudLowerLeftShiftXTitan();
void StepHudLowerLeftShiftX();
void StepHudWidgetScale();
void RuiWidgetUniformScale(unsigned rva, std::uint8_t* scratch);
// hud.canvas_centre: a screen-space widget with a full-space element is
// translated so that element is centred. See the block above its definition.
void RuiCanvasCentre(unsigned rva, std::uint8_t* scratch);
void SetHudCanvasCentre(bool on);

// `hud.widget_census`. READ-ONLY. Correlates every substituted widget's
// register positions against head yaw and pitch and reports each register as
// WORLD-ANCHORED (position tracks the head; may only be resized about its own
// anchor) or screen-anchored (safe to scale about the screen centre). The
// marker widget is the built-in positive control: R08 must come back
// world-anchored and R04 must not. Gated on the head actually having swept.
void SetRuiWidgetCensus(bool armed);
void RuiWidgetClassify(unsigned rva, std::uint8_t* scratch);
void ReportWidgetClassification(bool force);

// `hud.widget_skip`. One widget RVA (DECIMAL) excluded from `hud.widget_scale`,
// for an element the uniform scale is wrong for. 0 = none.
void SetHudWidgetSkip(unsigned rva);
// F6 THIS BUILD: steps `hud.widget_skip` through the widgets actually scaled
// this session, one per press, so the wearer can hand one widget at a time back
// to the game and see which one holds the jump/slide bars.
void StepHudWidgetSkip();
// `hud.widget_named_only`. 1 = move ONLY the three widgets the nomenclature table
// names (loadout block x2, earn meter); leave every other widget exactly where the
// game puts it, which is what the wearer asked for the two bars. F6 toggles it.
void SetHudWidgetNamedOnly(bool only);
bool HudWidgetNamedOnly();
void StepHudWidgetNamedOnly();

// `hud.widget_screenspace_only`. 1 (default) scales only widgets whose space is
// the 1920x1080 screen. Model-space widgets (Titan cockpit, painted on
// geometry) have their own 0.5 that is not the screen centre.
void SetHudWidgetScreenSpaceOnly(bool only);
// F6 THIS BUILD: toggles it live, so the Titan cockpit group can be moved for one
// press. It is the only widget group the wearer has never been able to reach.
void StepHudWidgetScreenSpaceOnly();
// `hud.cockpit_scale`. The Titan cockpit group.s own scale, separate from
// hud.widget_scale. 0 holds the group back entirely. F6 steps it this build.
void SetHudCockpitScale(float scale);
float HudCockpitScale();
void StepHudCockpitScale();
// `hud.cockpit_skip`. One cockpit widget RVA (DECIMAL) left entirely alone --
// 14848 is 0x3A00, the two-bar indicator the wearer wants untouched.
void SetHudCockpitSkip(unsigned rva);
// `hud.bars_scale`, `hud.bars_shift_y`. The two-bar indicator named by
// hud.cockpit_skip gets its own size multiple and its own height offset in its
// own space. 1.00 and 0.000 leave it exactly as the game draws it. F6 steps the
// height through BOTH directions -- the mesh orientation is not predicted.
// `hud.nameplate_scale`. The friend and enemy name labels (type-7 widgets).
// SIZE ONLY and scaled about the group's own centre, because a nameplate is
// world-anchored: it must stay on the entity it names, and the health bar under
// an enemy label must shrink with the label as one piece.
void NoteWidgetPath(unsigned rva, int path);
// `hud.marker_unzoom`. Cancels hud2d.zoom for the world-anchored markers on the
// pixel-ortho pass ONLY, so the lock markers land on their targets while the
// reticle and menus keep the shrink the wearer chose. F12 toggles it.
void RuiUnzoomWorldMarkers(unsigned rva, std::uint8_t* scratch);
void SetHudMarkerUnzoom(bool on);
bool HudMarkerUnzoom();
void ToggleHudMarkerUnzoom();
// READ-ONLY. Logs the printable strings a widget drew, once per widget per
// session, so a label can be identified by the text the wearer can read on
// screen rather than guessed at from register geometry.
void RuiWidgetStrings(unsigned rva, const std::uint8_t* scratch);
// F5 (2026-09-07): the next three draws of the friendly-label widget print their
// argument block and registers with the body and head angles. Read-only.
void RuiLabelDump(unsigned rva, const std::uint8_t* scratch);

void RuiNameplateScale(unsigned rva, std::uint8_t* scratch);
void SetHudNameplateScale(float scale);
float HudNameplateScale();
void StepHudNameplateScale();
// `hud.nameplate_mode`. 0 = size only (cannot drift). 1 = group scaled about the
// median position, so the label and its health bar shrink as one piece.
void SetHudNameplateMode(int mode);
int HudNameplateMode();
void StepHudNameplateMode();
void SetHudBarsScale(float scale);
void SetHudBarsShiftY(float shift);
float HudBarsScale();
float HudBarsShiftY();
void StepHudBarsShiftY();
