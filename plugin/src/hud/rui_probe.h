#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// READ-ONLY PROBE ON THE RUI DRAW CALL -- which layer is the HUD?
//
// Established, not assumed:
//   - the HUD is RUI (rui_drawEnable 0 blanks it, write read back as TOOK);
//   - engine.dll+0xFC500 is the RUI draw function Northstar detours to do that;
//   - it has exactly one call site, engine.dll+0xFC87A, a 5-byte E8 rel32.
//
// FC500 is per-layer setup, and its own instructions name the fields:
//
//   movzx ecx, byte [rdi+10h]   ; rcx = context     -> the LAYER TYPE byte
//   mov   r11, [rdx]            ; rdx = descriptor  -> a source struct
//   movss xmm7, [r11+18h]       ;                      two floats, which are
//   movss xmm6, [r11+1Ch]       ;                      shufps-broadcast into
//   mov   r10, [rdx+18h]        ;                      the output struct at
//   movaps [r10+2DD0h], xmm7    ;                      +2DD0h
//
// This logs exactly those, and writes nothing. The question it exists to answer
// is WHICH LAYER TYPE THE HUD IS, because the repeated failure in this project
// is scaling a pass that turned out not to be the HUD -- the removed knob that
// damaged the image, and the 64-byte matrix that moved 798 uploads and nothing
// on screen. The type byte is what finally makes the write selective.
//
// COST IS BOUNDED BY CONSTRUCTION, not by a cap someone has to remember. This
// runs per draw, so the steady-state cost is one byte read and a walk of a
// fixed 16-entry table. The pointer chase that reads the two floats happens
// only the FIRST time each type is seen -- at most sixteen times in a session,
// then never again. A per-draw probe that forgets this is how the framerate
// gets tanked and the run produces nothing.
//
// EVERY DEREFERENCE HERE IS ONE FC500 ITSELF PERFORMS, on the same pointers, a
// few instructions later. There is nothing to validate that the engine does not
// already validate by using it.
// ---------------------------------------------------------------------------

// Patches the call site. Reports what it verified and refuses on any mismatch.
// Hotkey-gated and never automatic: it patches code, and this project's rule is
// that anything touching the engine arms on a keypress.
void ToggleRuiProbe();

// Prints the per-type tally on its own clock -- not gated behind any UI mode,
// so a run that is never disarmed still leaves a readable instrument.
// The shipped control. Layer type and factor come from the INI and reload live
// with LEADER then END; the key is a straight A/B toggle, because a size judged
// by eye has to be seen with and without it back to back in one session.
void ToggleRuiLayerScale();
// SIZE is a per-edge INSET and POSITION is a bias between opposing edges, both
// written into the engine own effective-offset slot -- the one its safe-area
// path uses. A uniform inset shrinks about the CENTRE, which is what the
// coordinate-space scale could never do: that grew from the top-left origin and
// dragged the HUD into the corner.
void SetRuiInset(float v);
float RuiInset();
bool RuiLayerScaleOn();
// Writes the dialled numbers out in ini syntax, on its own key, so a session
// spent tuning by feel ends with something copyable instead of a memory.
void ReportRuiLayerScale();
// Isolates one layer type per press, logging which. See the note at the impl.
void IsolateNextRuiType();
void SetRuiLayerType(int type);
void AdvanceRuiProbe();

// Puts the original call back.
void RemoveRuiProbe();

// ---- arming on load -------------------------------------------------------
//
// SHIPPING THE HALF THAT WORKS. The top-left cluster's inset is measured,
// dialable and headset-verified, and it was still costing a keypress every
// session. `rui.arm = 1` in the INI arms it once, at the INI's own inset, as
// soon as the game is rendering a world -- and ONCE ONLY, because F6 has to
// stay a straight A/B against the original. A "wanted" flag that re-arms on a
// clock, which is how arms.collapse works, would undo the A/B on the next
// tick and make the key look broken.
void SetRuiArmOnLoad(bool wanted);
bool RuiArmOnLoad();

// THE MAGNITUDE HAS ITS OWN ARMING PATH and it is not the call-site patch.
// The inset's size rides `rui_safeAreaFrac`, written through the value slot --
// and the INI is parsed before engine.dll has registered a single cvar, so that
// write fails silently at load. Anything that arms without re-pushing it insets
// by the engine's own default frac instead of the dialled value. This reports
// whether a read-back agrees, so the falsifier can say so out loud.
bool RuiInsetCvarTook();
float RuiInsetCvarReadBack();

// ---- RUNG A1: the read-only identity census at the 0xFC6B1 seam -----------
//
// WHAT THE SEAM ACTUALLY IS, decoded rather than assumed (pescan fullfunc
// FC500, .pdata says one fragment FC500..FC6DA, so FC6B1 is inside the real
// function and not a chained continuation):
//
//   000FC50E  mov  r11, [rdx]          ; rdx = the LAYER, r11 = its DESCRIPTOR
//   000FC555  movss xmm7, [r11+0x18]   ; the authored coordinate space, w
//   000FC55B  movss xmm6, [r11+0x1C]   ;                                h
//   000FC6AE  mov  rdx, [rdi]
//   000FC6B1  call [r11+0x68]          ; <- the seam. FOUR BYTES: 41 FF 53 68.
//
// Two things follow, and they set this rung's whole shape.
//
// FIRST: the call site cannot be patched. It is four bytes and an E8 is five.
// That is why the plan forbids it -- not caution, arithmetic.
//
// SECOND, and this is the cheap part: r11 is not a vtable. Its +0x18/+0x1C are
// the floats the last census already measured (1920x1080, 256x256, 925x28), so
// it is a per-layer-class DESCRIPTOR STRUCT that happens to carry a function
// pointer at +0x68. And r11 is `*(void**)rdx` -- which means the descriptor,
// and the target it resolves to, are BOTH READABLE FROM THE THUNK WE ALREADY
// OWN, before FC500 is even entered. A1 needs no new patch at all.
//
// So the per-call identity is the DESCRIPTOR, which is a strictly finer grain
// than the layer-type byte: the type byte lives on the shared context at
// +0x10, one value for a whole batch, and it is what the failed sweeps keyed
// on. This also measures whether a finer grain than the descriptor exists, by
// counting the distinct LAYER objects behind each one -- so "is this grain
// fine enough" is answered in the same run rather than costing another.
//
// Read-only. Every dereference here is one FC500 performs on the same pointer
// a few instructions later.
void SetRuiCensusWanted(bool wanted);
bool RuiCensusWanted();

// rui.census_early. Arms the census before the world gate so the first continue
// panel is visible to it. Diagnostic; default off.
void SetRuiCensusEarly(bool early);
bool RuiCensusEarly();
// Installs the call-site patch if it is not already in, then arms the census.
void ArmRuiCensus();
// Dumps the census on its own five-second clock, so a run that is never
// disarmed still leaves a readable instrument -- and on demand.
void AdvanceRuiCensus();
void ReportRuiCensus();

// A labelled census dump taken at an instant the caller cares about, rather
// than on the periodic timer. See the definition.
void RequestRuiCensusSnapshot(int label);

// ---- RUNG A2: selective suppression by resolved draw target ---------------
// See the long note at the implementation. Steps a RANKED, NAMED candidate
// ladder -- the names come from the RUI asset strings each draw function in
// ui(11).dll references, read offline with pescan, so the sweep is not blind.
// Each arm publishes its own suppressed-call count BEFORE moving on: a null
// with a zero count is instrument failure, not a refutation.
void SuppressNextRuiCandidate();
// F3 advances the ladder from any state; F5 names the step the name vanished at.
void StepRuiWidgetHunt();
void MarkRuiWidgetHunt();
void ReportRuiSuppressionControl();

void AdvanceRuiSuppression();
std::uint64_t RuiSuppressedCalls();

// ---- RUNG A3: transform the lower-left group's own placement --------------
// Substitutes the descriptor's +0x68 draw-function slot -- a DATA write, the
// plan's preferred mechanism -- and rewrites the placement FC500 computed in
// the seven instructions before the seam. See the note at the implementation
// for the lane derivation. Steps a ladder whose FIRST arm is an identity
// transform: if that moves anything, the reading is wrong.
void StepRuiA3();
void AdvanceRuiA3();
void RemoveRuiA3();

// Which ladder F5 steps. A2's is answered, so F5 steps A3 by default -- but A2
// stays reachable from the ini rather than being deleted, because the health
// element itself was never on screen during the A2 run and may yet need it.
void SetRuiA2LadderSelected(bool selected);
bool RuiA2LadderSelected();

// ---- the shipped lower-left control --------------------------------------
// `rui.ll_scale` scales the lower-left group's coordinate space, which scales
// the group about the SCREEN CENTRE by its reciprocal -- smaller, off the
// bottom edge and in from the left, in one number. 1.0 is inert and is the
// default. `rui.ll_arm = 1` arms it on load, once, exactly like `rui.arm`.
void SetRuiLowerLeftScale(float scale);
float RuiLowerLeftScale();
void SetRuiLowerLeftArmOnLoad(bool wanted);
bool RuiLowerLeftArmOnLoad();
// Installs the descriptor substitution and applies the ini's scale. Separate
// from the ladder so shipping does not depend on anyone pressing a key.
void ArmRuiLowerLeft();
bool RuiLowerLeftArmed();
// Nudges the lower-left scale. Positive `steps` means BIGGER on screen, which
// is a SMALLER coordinate space -- the reciprocal is inverted here once, so
// nothing above this has to remember that it is inverted at all.
void NudgeRuiLowerLeft(int steps, bool fine);
void SetRuiA3LadderSelected(bool selected);
bool RuiA3LadderSelected();
bool RuiCensusArmed();

// ---- the sine-wave warp: does it reproduce FLAT? -------------------------
//
// The wearer reports the lower-left group riding up and down as the body
// rotates. That is a ROTATION-DEPENDENT symptom, and every test this project
// has run against the HUD has been STATIC -- including rung 1's hudwarp_*
// sweep, which concluded "writes land, nothing moves" while the HUD sat still.
// Rung 1 answered "does hudwarp MOVE the HUD"; it never asked "does hudwarp
// WARP it while turning", which is what the family is named for.
//
// Worse, that run was one of the three where flat-at-the-desk was silently
// broken: `autoarm = 4` forced 4032x2268 onto a 1440p monitor, so the bottom
// of the frame -- exactly where this group lives -- was off-screen. The
// handoff already flags every visual null from those runs as weaker than
// recorded. This one is weaker still, because it was aimed at the wrong
// question.
//
// So before any mechanism work: does the warp happen flat at all? If it does,
// it can be chased on the monitor for the price of a mouse turn. This captures
// a burst of full frames so the trajectory can be MEASURED rather than
// remembered -- a HUD that rides the view and one that sits still look similar
// through a headset and identical in a single screenshot.
void SetRuiWarpProbe(bool enabled);
bool RuiWarpProbeSelected();
void StartRuiWarpBurst();
void AdvanceRuiWarpBurst();

// ---- the sine-wave warp: a suspect ladder, run IN VR ----------------------
// One mechanism disarmed per press against an otherwise untouched baseline,
// with stage 1 as a positive control that the wobble is present at all. See
// the note at the implementation for what is already eliminated and how far.
void StepRuiSuspectLadder();
// F3: blank one widget per press until the lock rings disappear (rui_probe.cpp).
void StepRuiLockHunt();
// F5: capture the CURRENT arm again without advancing it (rui_probe.cpp).
void LogRuiLockHuntState();
// F3: blank the entire RUI system, to settle whether the rings are RUI at all.
void ToggleRuiDrawEnable();
// F3: turn off one draw system per press until the lock rings vanish.
void StepDrawSystemHunt();
// F3: print VGUI's own panel tree on screen so the lock panel names itself.
void ToggleVguiDrawTree();
void AdvanceRuiSuspectLadder();
void SetRuiSuspectLadder(bool selected);
bool RuiSuspectLadderSelected();

// ---- the parameter-block census -------------------------------------------
// The wobble is MEASURED to be in the game's backbuffer, so the thing to read
// is what the widget writes. Records the block at scratch+0x2DD0 over a turn
// and reports which floats moved. Read-only. See the note at the impl.
void StartRuiBlockCensus();
void AdvanceRuiBlockCensus();
void ReportRuiBlockCensus();
void SetRuiBlockCensusSelected(bool selected);
bool RuiBlockCensusSelected();
// One unprompted capture burst per session, so a visual bug always has a
// picture of it in the evidence rather than only a description.
void AdvanceRuiAutoShots();

// ---- the world-marker census ----------------------------------------------
// `rui.marker_target`. A ui(11).dll RVA to substitute (pass-through, nothing
// scaled) and point the block census at, so a WORLD-ANCHORED widget can be
// watched instead of the screen-anchored lower-left pair. 0 = none.
//
// Candidates, all confirmed drawing in the H3 run:
//   0x809B0  the distance-labelled world marker -- THE WAYPOINT, start here
//   0xBF60   the same family on layer type 4
//   0x69570  overhead_icon_titan_arrow_you
//   0xC4D0   battery capture friendly/enemy
//   0x5B800  lockon_indicator centre + edge -- the Titan missile lock
// `hud.widget_arm`. Substitutes EVERY type-3 widget the census names, so
// `hud.widget_scale` can reach the whole HUD instead of one widget. The eager
// path in the census hook covers first draws, so nothing comes up unscaled.
void SetRuiHudSizeArm(bool armed);
// `hud.widget_types`. DECIMAL bitmask of FC500 draw types the size control
// substitutes: bit0=1 bit3=8 bit4=16 bit7=128. 152 is types 3+4+7, where 7 is
// the friend and enemy name labels. Type 0 is the frontend and menus.
void SetRuiHudSizeTypes(unsigned mask);
unsigned RuiHudSizeTypes();
// F12 this build: flips hud.widget_scale between the wearer's 0.55 and 1.00 so a
// capture can be taken on each side of the same scene.
void ToggleHudWidgetScaleAB();
void SetRuiMarkerTarget(unsigned rva);
unsigned RuiMarkerTarget();

// F6 this build. Steps the marker ladder: one coordinate-space scale per press
// on the widget `rui.marker_target` names, first arm the identity. Tests the
// frustum-scale hypothesis for the mispointing world markers, visibly, in one
// run. Inert until pressed.
void StepRuiMarkerLadder();

// `hud.marker_size`. Scales the watched marker widget's coordinate space.
//
// SIZE ONLY, and that separation is measured rather than assumed: the
// 2026-09-05 scale ladder moved this same coordinate space across 3726 draws
// and changed the marker's SIZE while leaving the head-tracking error
// untouched. Restoring the HUD pass's uploaded frustum (hud.fov_match = 1)
// fixed the tracking and brought back a 1.8952x magnification with it, so the
// frustum owns where the marker sits and this owns how big it is.
// 1/1.8952 = 0.5277 cancels the magnification exactly. F5 steps the ladder.
void SetRuiMarkerSize(float scale);
float RuiMarkerSize();
void StepRuiMarkerSize();
