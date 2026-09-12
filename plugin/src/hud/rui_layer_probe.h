#pragma once
#include <cstdint>

// The widget identified by disappearance on 2026-09-07: it draws the friendly
// name label AND the enemy name label and health bar. Its args carry no
// transform and no world point, and nothing in its scratch tells one label kind
// from the other -- only the screen position it is handed differs.
constexpr unsigned kLabelWidgetRva = 0x8A330;

// Read-only, called from inside the substituted dispatch for EVERY substituted
// widget with that widget's own scratch. Accumulates, per RVA, how far its
// screen position moves per degree of HEAD yaw while the body is holding still.
// Screen-fixed widgets must read flat; world-anchored ones must agree with each
// other. That population is the reference, so no frustum mapping is assumed.
// stage 0 = sampled BEFORE our transforms (the game's own numbers), 1 = after.
void RuiLayerProbe(unsigned rva, const std::uint8_t* scratch, int stage);
// Rides on the classifier report so it cannot be silently absent from a log
// that otherwise looks whole.
void ReportRuiLayerProbe();
// F6. Logs every layer the label widget draws over the next few frames, each
// with its own position and the head pose at that draw. Two presses at two head
// yaws compare set against set, which needs no instance tracking at all -- and
// tracking is what three runs of regression could not do.
void RuiLayerProbeBurst();
