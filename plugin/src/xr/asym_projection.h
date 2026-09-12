#pragma once

// ---------------------------------------------------------------------------
// ASYMMETRIC PER-EYE FRUSTUM GEOMETRY. Pure functions, no engine dependencies,
// so `tools/asym-projection-check.cpp` compiles the SAME code the plugin runs
// and checks it against known headset numbers offline.
//
// PLAN-ASYMMETRIC-FOV-2026-08-28 requires the Y flip to be "verified by
// construction, not by eye", with an offline check fed the PFD MR's symmetric
// numbers and the Quest 3's asymmetric ones. A sign error here is invisible on
// a symmetric headset and wrong on every other one, so it must be provable
// without a headset. Code that cannot be compiled outside the plugin cannot be
// checked that way.
//
// TWO SPACES, AND THEY DISAGREE ABOUT WHICH WAY IS UP:
//   * TANGENT space is the frustum's. Up is POSITIVE. This is where projection
//     is linear, which is the only reason any of this arithmetic is simple.
//   * IMAGE space is the swapchain's. Down is POSITIVE, origin top-left.
// Every conversion between them lives in WindowToRect below and nowhere else.
//
// WHY A SHEAR AND NOT A SCALE (REPLAN-2026-08-28-MECHANISM-B section 4). The
// engine already renders a 103.71 degree vertical -- nearly the whole span the
// Quest 3's 99 degree lens needs -- but aimed 13 degrees wrong. Adding a
// multiple of the clip-w row to a clip row TRANSLATES the frustum without
// changing its size, so full coverage costs exactly nothing in draw calls, and
// the edit is a uniform NDC translation that can be applied to EVERY camera
// family without knowing anything about any of them. That last property is the
// point: `fit_horizontal` defaults OFF because scaling one family's row left
// the others behind, and the code that does it warns that "a heuristic set that
// has to be exhaustive to be correct will not stay correct". A translation
// needs no such set.
// ---------------------------------------------------------------------------

namespace tf2vr {

// A frustum's extents on the near plane, in tangent space. `up` and `right` are
// normally positive and `down` and `left` normally negative -- but NOT always:
// a headset whose optical axis points below forward can report an `up` that is
// still positive while its centre is negative, and one canted far enough could
// report both vertical extents on the same side. Nothing here assumes a sign.
struct TanExtents {
    float left = 0.0f;
    float right = 0.0f;
    float up = 0.0f;
    float down = 0.0f;
};

// A rectangle in IMAGE space: x right, y DOWN, origin top-left.
struct ImageRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// Symmetric about forward on BOTH axes, within epsilon. This is V2's falsifier
// predicate: the PFD MR reports perfectly symmetric optics, so every function
// below must be an identity on it, and the caller asserts exactly that.
inline bool IsSymmetric(const TanExtents& e, float epsilon) {
    const float cx = e.right + e.left;
    const float cy = e.up + e.down;
    return (cx < 0.0f ? -cx : cx) <= epsilon && (cy < 0.0f ? -cy : cy) <= epsilon;
}

// The NDC offset that moves a symmetric frustum of half-tangent `halfTan` so its
// centre lands on `lensCentreTan`.
//
// The engine writes clip = row . p, so adding `o * row3` to a clip row adds `o`
// to that axis' NDC: NDC = t/halfTan + o. NDC = 0 therefore moves to
// t = -o*halfTan, so the offset wanted is -lensCentreTan/halfTan. It returns
// exactly ZERO for a centred lens, which is V2's falsifier holding by
// construction rather than by a branch someone could forget to take.
//
// Capped at +/-0.5. An offset of 1 would push forward off the edge of the
// image; nothing sane reaches half that, and a runtime reporting nonsense must
// not be able to blank the view.
inline float ShearForCentre(float halfTan, float lensCentreTan) {
    if (halfTan <= 0.0001f) return 0.0f;
    float o = -lensCentreTan / halfTan;
    if (o > 0.5f) o = 0.5f;
    if (o < -0.5f) o = -0.5f;
    return o;
}

// Where a symmetric render of half-tangents (halfTanX, halfTanY) ends up after
// the shear above. THE SPAN IS UNCHANGED -- that is the whole reason to use a
// shear -- and only the centre moves.
inline TanExtents ShiftedExtents(float halfTanX, float halfTanY, float ndcX, float ndcY) {
    TanExtents out;
    out.right = (1.0f - ndcX) * halfTanX;
    out.left = (-1.0f - ndcX) * halfTanX;
    out.up = (1.0f - ndcY) * halfTanY;
    out.down = (-1.0f - ndcY) * halfTanY;
    return out;
}

// The part of the lens frustum that `rendered` actually contains. Intersection,
// not projection: an extent the render never reached cannot be declared, and an
// extent the lens never shows should not be.
inline TanExtents ClampToRendered(const TanExtents& lens, const TanExtents& rendered) {
    TanExtents out;
    out.left = lens.left < rendered.left ? rendered.left : lens.left;
    out.right = lens.right > rendered.right ? rendered.right : lens.right;
    out.up = lens.up > rendered.up ? rendered.up : lens.up;
    out.down = lens.down < rendered.down ? rendered.down : lens.down;
    return out;
}

// THE ONE PLACE THE Y AXIS FLIPS.
//
// `rect` is the image-space rectangle the world pass was rendered into, and it
// spans the tangent window `rendered`. This returns the sub-rectangle of it
// covering `window`, so the caller can declare `window` as the layer FOV and
// hand the compositor exactly the pixels that span it.
//
// Horizontal is the easy direction and both spaces agree: rendered.left is the
// LEFT edge, so the fraction across is (t - left) / (right - left).
//
// Vertical is the direction that gets written wrong. rendered.up is the TOP
// edge and image y counts DOWN from the top, so the fraction down is
// (up - t) / (up - down) -- the subtraction is the flip. The consequence worth
// stating, because it is the offline check's assertion: when the window's `up`
// is BELOW the rendered `up` (a lens whose optical axis points below forward,
// like the Quest 3's), the top edge of the sub-rect moves DOWN the image. Get
// the sign backwards and it moves up, which crops the wrong end -- invisible on
// a symmetric headset, wrong on every other one.
inline ImageRect WindowToRect(const ImageRect& rect, const TanExtents& rendered,
                              const TanExtents& window) {
    ImageRect out = rect;
    const float spanX = rendered.right - rendered.left;
    const float spanY = rendered.up - rendered.down;
    if (spanX > 0.0001f) {
        out.x = rect.x + rect.w * ((window.left - rendered.left) / spanX);
        out.w = rect.w * ((window.right - window.left) / spanX);
    }
    if (spanY > 0.0001f) {
        out.y = rect.y + rect.h * ((rendered.up - window.up) / spanY);   // the flip
        out.h = rect.h * ((window.up - window.down) / spanY);
    }
    return out;
}

}  // namespace tf2vr
