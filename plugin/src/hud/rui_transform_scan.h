#pragma once
#include <cmath>

// FINDING A TRANSFORM IN A BLOCK OF FLOATS, STRUCTURALLY.
//
// The alternative was "args A04-A06 is a matrix because the sibling widget's
// was", which is the kind of premise this front has already paid for twice --
// an RVA labelled from a doc and a widget named from a table. Three consecutive
// 4-float args whose xyz parts are all unit length is a basis, and that test
// does not care which widget, which build, or which document said what.
//
// In a header so it can be positive-controlled offline against args this repo
// has actually logged, the way rui_decline.h and rui_text_filter.h are.

inline float ArgRowLength(const float* r) {
    return std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
}

// Tolerance is deliberately tight. A loose one turns any three rows of similar
// magnitude into a "transform", and a PASS that accepts junk looks exactly like
// a real find.
inline bool ArgTripleIsBasis(const float* r0, const float* r1, const float* r2,
                             float tolerance = 0.02f) {
    return std::fabs(ArgRowLength(r0) - 1.0f) <= tolerance &&
           std::fabs(ArgRowLength(r1) - 1.0f) <= tolerance &&
           std::fabs(ArgRowLength(r2) - 1.0f) <= tolerance;
}

// The yaw a basis row points at, in the game's degrees.
inline float ArgRowYawDeg(const float* r) {
    return std::atan2(r[1], r[0]) * 57.2957795f;
}

// Signed difference between two yaws, wrapped into (-180, 180]. Without this a
// label at yaw 179 and a camera at yaw -179 read as 358 degrees apart.
inline float YawDeltaDeg(float a, float b) {
    float d = a - b;
    while (d > 180.0f) d -= 360.0f;
    while (d <= -180.0f) d += 360.0f;
    return d;
}

// NON-OVERLAPPING, which the first version was not.
//
// Two transforms stored back to back are six consecutive unit rows, and a
// sliding window of three finds FOUR of them -- at rows 0, 1, 2 and 3 of the
// run. Three of those four are half of one transform glued to half of the
// other: rows that are individually unit length and jointly meaningless. The
// log would have carried four "transforms" per dump, two of them fictional, and
// their yaws would have been read as evidence.
//
// So: walk the maximal runs of consecutive unit rows, and take transforms three
// at a time from the START of each run. `args` is `count` rows of four floats.
// Returns how many were found; writes their starting row indices to `out`.
inline int ScanArgTransforms(const float* args, int count, int* out, int max) {
    int found = 0;
    int i = 0;
    while (i + 2 < count) {
        if (std::fabs(ArgRowLength(args + 4 * i) - 1.0f) > 0.02f) { ++i; continue; }
        int run = 0;
        while (i + run < count &&
               std::fabs(ArgRowLength(args + 4 * (i + run)) - 1.0f) <= 0.02f) ++run;
        for (int k = 0; k + 2 < run + 1 && k + 3 <= run; k += 3) {
            if (found < max) out[found] = i + k;
            ++found;
        }
        i += run;
    }
    return found;
}
