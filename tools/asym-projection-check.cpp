// ---------------------------------------------------------------------------
// OFFLINE CHECK for plugin/src/asym_projection.h. No headset, no game, no
// build of the plugin -- it compiles the SAME header the plugin includes.
//
// PLAN-ASYMMETRIC-FOV-2026-08-28: "Verify the Y flip by construction, not by
// eye. Write it as an offline check with known numbers BEFORE it goes near a
// headset: feed the PFD MR's symmetric numbers and assert the rect is the full
// image; feed the Quest 3's and assert the top edge moves DOWN in image space
// when tanU is the smaller magnitude."
//
// Build and run, from this directory:
//   cl /EHsc /std:c++17 /nologo /I..\plugin\src asym-projection-check.cpp
//   .\asym-projection-check.exe
//
// Every headset number below is READ OUT OF AN ARCHIVED BANNER, not invented:
// grep -a "HEADSET: runtime" in the rotated titanfall2vr logs.
// ---------------------------------------------------------------------------

#include "asym_projection.h"

#include <cmath>
#include <cstdio>

using namespace tf2vr;

static int g_failures = 0;
static int g_checks = 0;

static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("  %s  %s\n", ok ? "pass" : "FAIL", what);
}

static void Close(float got, float want, float tolerance, const char* what) {
    const bool ok = std::fabs(got - want) <= tolerance;
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("  %s  %-56s got %.5f want %.5f\n", ok ? "pass" : "FAIL", what, got, want);
}

static float Tan(float degrees) { return std::tan(degrees * 3.14159265358979f / 180.0f); }
static float Deg(float tangent) { return std::atan(tangent) * 180.0f / 3.14159265358979f; }

// THE NEGATIVE CONTROL. The same conversion with the Y flip left out -- image y
// counted UP from the bottom instead of down from the top. This is the bug the
// plan warns about, written out so the assertions below can be shown to CATCH
// it. An assertion that has never failed is not known to be able to.
static ImageRect WindowToRectNoFlip(const ImageRect& rect, const TanExtents& rendered,
                                    const TanExtents& window) {
    ImageRect out = rect;
    const float spanX = rendered.right - rendered.left;
    const float spanY = rendered.up - rendered.down;
    if (spanX > 0.0001f) {
        out.x = rect.x + rect.w * ((window.left - rendered.left) / spanX);
        out.w = rect.w * ((window.right - window.left) / spanX);
    }
    if (spanY > 0.0001f) {
        out.y = rect.y + rect.h * ((window.down - rendered.down) / spanY);   // NO FLIP -- wrong
        out.h = rect.h * ((window.up - window.down) / spanY);
    }
    return out;
}

int main() {
    // -----------------------------------------------------------------------
    // 1. THE PFD MR -- perfectly symmetric, and therefore the falsifier.
    //    Banner: eye0 fov L-55.00 R55.00 U45.00 D-45.00, centre offset
    //    x=0.0000 y=0.0000. Renders 90 deg vertical, so halfTanY = tan 45.
    //
    //    V2: "when |tanU| == |tanD| and |tanL| == |tanR| within epsilon, assert
    //    the computed sub-rect equals the pre-existing rect and the declared FOV
    //    equals the previous values."
    // -----------------------------------------------------------------------
    std::printf("PFD MR (symmetric) -- a correct implementation CANNOT alter it\n");
    {
        TanExtents lens;
        lens.left = Tan(-55.0f);
        lens.right = Tan(55.0f);
        lens.up = Tan(45.0f);
        lens.down = Tan(-45.0f);
        const float halfTanX = Tan(55.0f);
        const float halfTanY = Tan(45.0f);

        Check(IsSymmetric(lens, 1e-4f), "reported optics are symmetric within 1e-4");

        // THE SHEAR MUST BE EXACTLY ZERO. Not small -- zero.
        const float shearY = ShearForCentre(halfTanY, 0.5f * (lens.up + lens.down));
        const float shearX = ShearForCentre(halfTanX, 0.5f * (lens.right + lens.left));
        Close(shearY, 0.0f, 0.0f, "vertical shear is EXACTLY zero");
        Close(shearX, 0.0f, 0.0f, "horizontal shear is EXACTLY zero");

        const TanExtents rendered = ShiftedExtents(halfTanX, halfTanY, shearX, shearY);
        Close(rendered.up, halfTanY, 1e-6f, "unsheared render keeps its top");
        Close(rendered.down, -halfTanY, 1e-6f, "unsheared render keeps its bottom");

        const TanExtents window = ClampToRendered(lens, rendered);
        Close(Deg(window.up), 45.0f, 1e-3f, "declared FOV up == the previous value");
        Close(Deg(window.down), -45.0f, 1e-3f, "declared FOV down == the previous value");
        Close(Deg(window.left), -55.0f, 1e-3f, "declared FOV left == the previous value");
        Close(Deg(window.right), 55.0f, 1e-3f, "declared FOV right == the previous value");

        // The pre-existing letterbox rect, which the new rect must COMPOSE with
        // rather than replace. On this headset it must come back untouched --
        // and it is checked at a NON-zero origin with a NON-square shape,
        // because a full rect at the origin hides both a dropped offset and a
        // w/h transposition.
        ImageRect rect;
        rect.x = 128.0f; rect.y = 64.0f; rect.w = 4032.0f; rect.h = 2520.0f;
        const ImageRect got = WindowToRect(rect, rendered, window);
        Close(got.x, rect.x, 1e-3f, "sub-rect x == the pre-existing rect");
        Close(got.y, rect.y, 1e-3f, "sub-rect y == the pre-existing rect");
        Close(got.w, rect.w, 1e-3f, "sub-rect w == the pre-existing rect");
        Close(got.h, rect.h, 1e-3f, "sub-rect h == the pre-existing rect");
    }

    // -----------------------------------------------------------------------
    // 2. THE QUEST 3 -- asymmetric on BOTH axes, and the horizontal asymmetry
    //    is the LARGER of the two and mirrors between the eyes.
    //    Banner: eye0 L-54.00 R40.00 U44.00 D-55.00, eye1 L-40.00 R54.00,
    //    centre offset x=-0.2425 y=-0.1932.
    //    Live render: game frustum tan 1.37623 x 1.27325.
    //
    //    Only the VERTICAL is sheared. The horizontal lens is already covered
    //    to 0.006 deg by the symmetric render, so shearing it would buy no
    //    coverage while changing stereo -- two variables for one run.
    // -----------------------------------------------------------------------
    std::printf("\nQuest 3 eye0 -- vertical shear for full coverage\n");
    {
        const float halfTanX = 1.37623f;
        const float halfTanY = 1.27325f;
        TanExtents lens;
        lens.left = Tan(-54.0f);
        lens.right = Tan(40.0f);
        lens.up = Tan(44.0f);
        lens.down = Tan(-55.0f);

        Check(!IsSymmetric(lens, 1e-4f), "reported optics are NOT symmetric");
        Check(std::fabs(lens.up) < std::fabs(lens.down), "tanU is the smaller magnitude");

        // BEFORE: the symmetric render misses the bottom of the lens.
        const TanExtents before = ShiftedExtents(halfTanX, halfTanY, 0.0f, 0.0f);
        Check(before.down > lens.down, "unsheared: the render stops ABOVE the lens bottom");
        Close(Deg(before.down) - Deg(lens.down), 3.14f, 0.02f,
              "unsheared: degrees of missing content at the bottom");

        const float shearY = ShearForCentre(halfTanY, 0.5f * (lens.up + lens.down));
        Close(shearY, 0.18160f, 5e-5f, "vertical shear (NDC) for this lens");

        const TanExtents rendered = ShiftedExtents(halfTanX, halfTanY, 0.0f, shearY);

        // THE FEATURE: every degree the headset supports, and with margin left
        // over. The span is unchanged, so this costs nothing to render.
        Check(rendered.down < lens.down, "sheared: the render now reaches BELOW the lens bottom");
        Check(rendered.up > lens.up, "sheared: the render still covers the lens top");
        Close(rendered.up - rendered.down, 2.0f * halfTanY, 1e-5f,
              "sheared span is IDENTICAL -- no extra frustum");

        const TanExtents window = ClampToRendered(lens, rendered);
        Close(Deg(window.up), 44.0f, 1e-3f, "declared up == the lens, exactly");
        Close(Deg(window.down), -55.0f, 1e-3f, "declared down == the lens, exactly");
        Close(Deg(window.right), 40.0f, 1e-3f, "declared right == the lens, exactly");
        Check(Deg(window.left) > -54.0f, "declared left clamps to the render (0.006 deg short)");
        Close(0.5f * (Deg(window.up) + Deg(window.down)), -5.5f, 0.01f,
              "declared vertical centre, degrees below forward");

        ImageRect rect;
        rect.x = 0.0f; rect.y = 0.0f; rect.w = 3528.0f; rect.h = 3264.0f;
        const ImageRect got = WindowToRect(rect, rendered, window);

        // THE ASSERTION THE PLAN ASKS FOR, and it is a strict inequality.
        Check(got.y > rect.y + 1.0f,
              "top edge moves DOWN the image when the lens top is below the render top");
        Check(got.y + got.h < rect.y + rect.h - 1.0f,
              "bottom edge moves UP -- the render now overshoots the lens both ways");
        Check(got.x < rect.x + 1.0f, "left edge stays put (the render is the limit there)");
        Check(got.w < rect.w, "right edge crops -- the lens stops at +40 deg");
        Check(got.w > 0.0f && got.h > 0.0f, "sub-rect is non-degenerate");

        // A PROPERTY WORTH STATING, because it is also a TRAP: once the shear
        // has centred the render on the lens, the crop at the top and the crop
        // at the bottom are EQUAL -- both spans are symmetric about the same
        // centre. On this input the flipped and unflipped formulas therefore
        // agree exactly, and the Y flip is undetectable.
        //
        // That is precisely the "invisible on a symmetric headset" failure the
        // plan warns about, wearing a different hat: the shear manufactures the
        // symmetry. So the negative control CANNOT be run here.
        const ImageRect same = WindowToRectNoFlip(rect, rendered, window);
        Close(same.y, got.y, 1e-3f, "sheared: flipped and unflipped AGREE -- no control here");

        // THE NEGATIVE CONTROL, run where the crops are unequal: the shear-OFF
        // arm, which is a real shipping path (the toggle, and any headset whose
        // lens is taller than the render). The whole crop is at the top there,
        // so the flip is the difference between moving the top edge down by
        // 394 pixels and not moving it at all.
        const TanExtents unsheared = ShiftedExtents(halfTanX, halfTanY, 0.0f, 0.0f);
        const TanExtents offWindow = ClampToRendered(lens, unsheared);
        const ImageRect offGot = WindowToRect(rect, unsheared, offWindow);
        const ImageRect offBad = WindowToRectNoFlip(rect, unsheared, offWindow);
        Check(offGot.y > rect.y + 1.0f, "shear off: top edge still moves DOWN");
        Close(offGot.y + offGot.h, rect.y + rect.h, 1e-3f,
              "shear off: bottom edge pinned -- the render ends there");
        Check(!(offBad.y > rect.y + 1.0f),
              "control: the UNFLIPPED formula fails that assertion, so it can fail");
    }

    // -----------------------------------------------------------------------
    // 3. A THIRD HEADSET, never connected. Whatever it reports, the code must
    //    contain no per-headset branch -- so a synthetic third profile has to
    //    behave. Vertical asymmetry the OTHER way: optics aimed ABOVE forward.
    // -----------------------------------------------------------------------
    std::printf("\nSynthetic third headset (asymmetric UPWARD) -- no per-headset branch\n");
    {
        const float halfTanY = 1.27325f;
        TanExtents lens;
        lens.left = Tan(-50.0f);
        lens.right = Tan(50.0f);
        lens.up = Tan(55.0f);
        lens.down = Tan(-44.0f);
        const float shearY = ShearForCentre(halfTanY, 0.5f * (lens.up + lens.down));
        Check(shearY < 0.0f, "the shear reverses sign with the asymmetry");
        const TanExtents rendered = ShiftedExtents(1.27325f, halfTanY, 0.0f, shearY);
        Check(rendered.up > lens.up, "sheared: covers a lens that aims UP");
        ImageRect rect;
        rect.x = 0.0f; rect.y = 0.0f; rect.w = 4000.0f; rect.h = 3000.0f;
        const TanExtents window = ClampToRendered(lens, rendered);
        const ImageRect got = WindowToRect(rect, rendered, window);
        Check(got.y > rect.y + 1.0f, "top edge still moves DOWN, by less");
        Check(got.y + got.h < rect.y + rect.h - 1.0f, "bottom edge moves UP, by more");
    }

    // -----------------------------------------------------------------------
    // 4. A RUNTIME REPORTING NONSENSE MUST NOT BE ABLE TO BLANK THE VIEW.
    // -----------------------------------------------------------------------
    std::printf("\nDegenerate input -- the cap, and no division by zero\n");
    {
        Close(ShearForCentre(1.0f, 10.0f), -0.5f, 1e-6f, "absurd lens centre is capped");
        Close(ShearForCentre(1.0f, -10.0f), 0.5f, 1e-6f, "absurd lens centre is capped, other way");
        Close(ShearForCentre(0.0f, 1.0f), 0.0f, 1e-6f, "no half-tangent yet -> no shear");
        TanExtents zero;
        ImageRect rect;
        rect.x = 7.0f; rect.y = 9.0f; rect.w = 100.0f; rect.h = 50.0f;
        const ImageRect got = WindowToRect(rect, zero, zero);
        Close(got.w, rect.w, 1e-6f, "a zero-span render leaves the rect alone");
        Close(got.h, rect.h, 1e-6f, "a zero-span render leaves the rect alone (h)");
    }

    std::printf("\n%d checks, %d failures.\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
