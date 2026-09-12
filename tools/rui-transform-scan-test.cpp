// Positive control for the transform scanner, against args this repo has
// actually logged -- not synthetic ones.
//
// The rows below are verbatim from the arg dump of the sibling per-entity type-7
// widget ui(11)+0x84070 in %TEMP%\titanfall2vr.prev-7.log, LABEL DUMP #1, whose
// header reads: body <p-4.04 y99.40> head <p13.38 y96.09> delta y-3.31.
//
// The scanner must find BOTH triples there, must not invent a third out of the
// coordinate-space args that precede them, and must report a yaw whose relation
// to the logged body and head angles is stable -- because that relation is the
// entire measurement the next run is being spent on.

#include "../plugin/src/rui_transform_scan.h"

#include <cmath>
#include <cstdio>

namespace {

int passed = 0, total = 0;
void Check(bool ok, const char* what) {
    ++total;
    if (ok) ++passed; else std::printf("FAIL %s\n", what);
}

// A00..A09 exactly as logged, four floats each.
const float kArgs[10][4] = {
    {1920.0f, 1920.0f, 1080.0f, 1080.0f},          // A00 coordinate space
    {1920.0f, 1920.0f, 1080.0f, 1080.0f},          // A01
    {1920.0f, 1920.0f, 1080.0f, 1080.0f},          // A02
    {0.0f, 0.0f, 28.0f, 28.0f},                    // A03
    { 0.570412f,  0.34598f,  -0.744935f, -1236.47f},  // A04 |
    {-0.821193f,  0.258445f, -0.508771f,  1726.99f},  // A05 |- transform 1
    { 0.294084f,  0.901945f,  0.316245f, -10437.9f},  // A06 |
    {-0.0538247f, 0.34598f,  -0.936697f, -1231.05f},  // A07 |
    {-0.954263f,  0.258445f,  0.150294f,  1718.97f},  // A08 |- transform 2
    { 0.294078f,  0.827463f,  0.478354f, -10436.6f},  // A09 |
};

}  // namespace

int main() {
    // ---- the rows really are unit length, which is what the test rests on ---
    for (int i = 4; i <= 9; ++i) {
        char what[64];
        std::snprintf(what, sizeof(what), "logged row A%02d is unit length", i);
        Check(std::fabs(ArgRowLength(kArgs[i]) - 1.0f) <= 0.02f, what);
    }

    // ---- the scan finds exactly the two transforms and no others -----------
    // A naive sliding window finds FOUR here -- at A04, A05, A06 and A07 --
    // because six consecutive unit rows contain four windows of three. Three of
    // those are half of one transform glued to half of the next.
    int at[8]{};
    const int found = ScanArgTransforms(&kArgs[0][0], 10, at, 8);
    Check(found == 2, "exactly two transforms are found, not four overlapping windows");
    Check(found >= 1 && at[0] == 4, "the first starts at A04");
    Check(found >= 2 && at[1] == 7, "the second starts at A07");

    // The sliding-window behaviour that made this necessary, asserted directly
    // so a future simplification cannot quietly reintroduce it.
    int windows = 0;
    for (int i = 0; i + 2 < 10; ++i)
        if (ArgTripleIsBasis(kArgs[i], kArgs[i + 1], kArgs[i + 2])) ++windows;
    Check(windows == 4, "a sliding window really would have reported four");

    // ---- the coordinate space is NOT mistaken for one ----------------------
    // A00-A02 are 1920/1080 quads. A scanner loose enough to take those would
    // report a transform in every widget in the game.
    Check(!ArgTripleIsBasis(kArgs[0], kArgs[1], kArgs[2]),
          "the coordinate-space args are not read as a transform");
    Check(!ArgTripleIsBasis(kArgs[1], kArgs[2], kArgs[3]),
          "a mixed run spanning the space args is not read as a transform");

    // ---- the two transforms differ, and by a lot ---------------------------
    const float y1 = ArgRowYawDeg(kArgs[4]);
    const float y2 = ArgRowYawDeg(kArgs[7]);
    Check(std::fabs(YawDeltaDeg(y1, y2)) > 30.0f,
          "the two logged transforms are far apart in yaw, so which one a layer "
          "gets is a distinction worth measuring");

    // ---- yaw wrapping, because a label at 179 and a camera at -179 are close
    Check(std::fabs(YawDeltaDeg(179.0f, -179.0f) - 358.0f) > 350.0f,
          "wrapping does not return the naive difference");
    Check(std::fabs(YawDeltaDeg(179.0f, -179.0f) - (-2.0f)) < 0.001f,
          "179 and -179 are two degrees apart");
    Check(std::fabs(YawDeltaDeg(-179.0f, 179.0f) - 2.0f) < 0.001f, "and symmetrically");
    Check(std::fabs(YawDeltaDeg(10.0f, 10.0f)) < 0.001f, "identical yaws differ by zero");

    // ---- a rotation matrix stays found however it is oriented --------------
    for (int deg = -180; deg <= 180; deg += 15) {
        const float a = static_cast<float>(deg) * 0.0174532925f;
        const float m[3][4] = {
            { std::cos(a),  std::sin(a), 0.0f, 100.0f},
            {-std::sin(a),  std::cos(a), 0.0f, 200.0f},
            { 0.0f,         0.0f,        1.0f, 300.0f},
        };
        if (!ArgTripleIsBasis(m[0], m[1], m[2])) {
            Check(false, "a pure yaw rotation is recognised at every angle");
            break;
        }
        if (std::fabs(YawDeltaDeg(ArgRowYawDeg(m[0]), static_cast<float>(deg))) > 0.01f) {
            Check(false, "and its row-0 yaw reads back as the angle it was built from");
            break;
        }
    }
    Check(true, "a pure yaw rotation is recognised and read back at every angle");

    std::printf("%d/%d passed\n", passed, total);
    return passed == total ? 0 : 1;
}
