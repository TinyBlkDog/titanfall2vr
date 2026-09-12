#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// THE FRUSTUM CENSUS. Read-only, no keys, no wearer judgement.
//
// WHERE THIS SITS IN THE ARGUMENT. Flat is perfect, so the defect is ours. The
// camera write is faithful (anchor probe: residual mean 0.153 deg, yaw bins
// flat, no tilt) and the aim is faithful (0.1 deg of aim against 24 deg of
// head). Yet everything screen-anchored -- waypoint AND reticle -- drifts
// cyclically under rotation, in the shape of a SCALE about the view centre:
// zero at the middle, growing outward, bounded, translation-immune, stable when
// you stop. A correct camera orientation is therefore being turned into a
// screen position through a frustum that is not the one the eye is displayed
// with, and the ratio between those two frustums IS the scale.
//
// So: every camera constant-buffer upload carries a camera-relative-to-clip
// matrix. For a perspective projection its diagonal gives the half-tangents
// directly (tanX = 1/m00, tanY = 1/m11), and the last row says whether it is
// perspective at all. This buckets every upload by those numbers plus the
// camera's own origin magnitude, and prints each distinct family with its
// count.
//
// WHAT IT DECIDES. If one family carries the headset's tangents (about 1.428 x
// 1.000) and another carries something else -- 16:9's 1.778, or the flat
// default -- then screen-space content projected through the second and
// displayed through the first is scaled by their ratio, and that ratio is the
// defect, measured rather than fitted. If every perspective family agrees, the
// frustums are not it and the scale is applied somewhere after projection.
//
// The families are not named by a predicate that could be wrong; they are named
// by their own numbers, so a family nobody predicted still shows up.
// ---------------------------------------------------------------------------

void FrustumCensusSample(const std::uint8_t* cameraBytes);
void FrustumCensusReport();
void SetFrustumCensusEnabled(bool enabled);
