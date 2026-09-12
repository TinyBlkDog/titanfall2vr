// BUILD: cl /nologo /std:c++20 /EHsc /O2 bmpshift.cpp /Fe:bmpshift.exe
//        (from a VS Developer prompt; the .exe is deliberately not committed)

// bmpshift -- measure the horizontal disparity between two same-frame captures,
// per depth band, and REFUSE to report anything until it has proved on a known
// shift that it can find one.
//
// WHY THIS EXISTS. Three disparity readings were taken in PowerShell and all
// three were wrong in the same silent way: PowerShell variable names are
// case-insensitive, so a band width `$w` and an image width `$W` are ONE
// variable. The band wrapped across rows, the correlation curve went flat, and
// the argmin sat at 0. Nothing errored. "Zero disparity in every band, SAD
// identical to two decimals" was reported as a finding when the instrument had
// never been shown capable of finding a shift at all.
//
// So this tool opens by shifting one image against ITSELF by a known number of
// pixels and checking it recovers that number. If the self-test fails, it
// prints the failure and exits non-zero without measuring anything.
//
// TWO OTHER PROPERTIES THE POWERSHELL VERSION LACKED:
//
//  - It matches on horizontal GRADIENTS, not raw luma. The two eyes come from
//    different render targets and the second one's grade is degenerate (its
//    post chain reads the engine's composite while writing ours), so their
//    absolute brightness differs by a lot. Edges survive that; levels do not.
//  - It searches dy as well as dx, and prints both. A pure lateral eye
//    translation needs dy = 0. A best match that only works with a vertical
//    offset is not an eye translation, and saying "dx = N" without that check
//    would hide it.
//
// It prints the whole curve's shape around the minimum, not just the argmin,
// because a boundary hit and a periodic texture both masquerade as a confident
// answer.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

namespace {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> gray;   // one byte per pixel
};

bool LoadBmp(const char* path, Image& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("cannot open %s\n", path); return false; }
    unsigned char fileHeader[14]{};
    unsigned char infoHeader[40]{};
    if (std::fread(fileHeader, 1, 14, f) != 14 || std::fread(infoHeader, 1, 40, f) != 40) {
        std::printf("%s: short header\n", path); std::fclose(f); return false;
    }
    if (fileHeader[0] != 'B' || fileHeader[1] != 'M') {
        std::printf("%s: not a BMP\n", path); std::fclose(f); return false;
    }
    int width = 0, height = 0;
    std::memcpy(&width, infoHeader + 4, 4);
    std::memcpy(&height, infoHeader + 8, 4);
    short bits = 0;
    std::memcpy(&bits, infoHeader + 14, 2);
    if (bits != 32) { std::printf("%s: expected 32bpp, got %d\n", path, bits); std::fclose(f); return false; }
    const bool topDown = height < 0;
    const int h = topDown ? -height : height;
    unsigned int offset = 0;
    std::memcpy(&offset, fileHeader + 10, 4);
    std::fseek(f, static_cast<long>(offset), SEEK_SET);
    std::vector<unsigned char> row(static_cast<size_t>(width) * 4);
    out.width = width;
    out.height = h;
    out.gray.assign(static_cast<size_t>(width) * h, 0);
    for (int y = 0; y < h; ++y) {
        if (std::fread(row.data(), 1, row.size(), f) != row.size()) {
            std::printf("%s: short pixel data at row %d\n", path, y); std::fclose(f); return false;
        }
        const int dst = topDown ? y : (h - 1 - y);
        for (int x = 0; x < width; ++x) {
            // Green channel: the captures are BGRA and green carries most of
            // the luminance detail. One channel keeps the match cheap and is
            // enough for edges.
            out.gray[static_cast<size_t>(dst) * width + x] = row[static_cast<size_t>(x) * 4 + 1];
        }
    }
    std::fclose(f);
    return true;
}

int Grad(const Image& im, int x, int y) {
    const unsigned char* r = &im.gray[static_cast<size_t>(y) * im.width];
    return static_cast<int>(r[x + 3]) - static_cast<int>(r[x - 3]);
}

struct Match { int dx = 0; int dy = 0; double sad = 0.0; bool boundary = false; long long samples = 0; };

Match Search(const Image& a, const Image& b, int bx, int by, int bw, int bh,
             int maxDx, int maxDy, int step) {
    Match best;
    best.sad = 1e30;
    for (int dy = -maxDy; dy <= maxDy; dy += step) {
        for (int dx = -maxDx; dx <= maxDx; dx += step) {
            double sum = 0.0;
            long long n = 0;
            for (int y = by; y < by + bh; y += 3) {
                const int y2 = y + dy;
                if (y < 4 || y >= a.height - 4 || y2 < 4 || y2 >= b.height - 4) continue;
                for (int x = bx; x < bx + bw; x += 3) {
                    const int x2 = x + dx;
                    if (x < 4 || x >= a.width - 4 || x2 < 4 || x2 >= b.width - 4) continue;
                    sum += std::abs(Grad(a, x, y) - Grad(b, x2, y2));
                    ++n;
                }
            }
            if (n < 100) continue;
            const double v = sum / static_cast<double>(n);
            if (v < best.sad) { best.sad = v; best.dx = dx; best.dy = dy; best.samples = n; }
        }
    }
    best.boundary = std::abs(best.dx) >= maxDx || std::abs(best.dy) >= maxDy;
    return best;
}

// THE SELF-TEST. Shift an image against itself by a known amount and check the
// search recovers it. An elimination instrument that has never fired is not
// evidence.
bool SelfTest(const Image& a) {
    const int bx = a.width / 4, by = a.height / 3;
    const int bw = a.width / 3, bh = a.height / 5;
    bool ok = true;
    for (int truth : {0, -37, 91}) {
        Image shifted;
        shifted.width = a.width;
        shifted.height = a.height;
        shifted.gray.assign(a.gray.size(), 0);
        for (int y = 0; y < a.height; ++y) {
            for (int x = 0; x < a.width; ++x) {
                const int src = x - truth;
                if (src < 0 || src >= a.width) continue;
                shifted.gray[static_cast<size_t>(y) * a.width + x] =
                    a.gray[static_cast<size_t>(y) * a.width + src];
            }
        }
        const Match m = Search(a, shifted, bx, by, bw, bh, 160, 24, 1);
        const bool pass = m.dx == truth && m.dy == 0;
        std::printf("  self-test: known dx=%+4d -> found dx=%+4d dy=%+3d  %s\n",
                    truth, m.dx, m.dy, pass ? "PASS" : "*** FAIL ***");
        if (!pass) ok = false;
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: bmpshift <eye0.bmp> <eye1.bmp> [x y w h]...\n"
                    "  With no bands, uses five spanning the frame.\n");
        return 2;
    }
    Image a, b;
    if (!LoadBmp(argv[1], a) || !LoadBmp(argv[2], b)) return 2;
    if (a.width != b.width || a.height != b.height) {
        std::printf("size mismatch: %dx%d vs %dx%d\n", a.width, a.height, b.width, b.height);
        return 2;
    }
    std::printf("bmpshift %dx%d\n%s\n  vs\n%s\n", a.width, a.height, argv[1], argv[2]);
    std::printf("SELF-TEST (the instrument must find a shift before it may report one):\n");
    if (!SelfTest(a)) {
        std::printf("SELF-TEST FAILED -- refusing to measure. Nothing below would mean anything.\n");
        return 1;
    }
    std::printf("self-test passed; the search can find a shift.\n\n");

    struct Band { const char* name; int x, y, w, h; };
    std::vector<Band> bands;
    if (argc >= 7) {
        for (int i = 3; i + 3 < argc; i += 4) {
            bands.push_back({"band", std::atoi(argv[i]), std::atoi(argv[i + 1]),
                             std::atoi(argv[i + 2]), std::atoi(argv[i + 3])});
        }
    } else {
        const int W = a.width, H = a.height;
        bands = {
            {"lower-left  (nearest)", W / 4,      H * 3 / 4, W / 3, H / 8},
            {"lower-centre",          W * 3 / 8,  H * 7 / 10, W / 4, H / 8},
            {"centre",                W * 3 / 8,  H / 2,     W / 4, H / 8},
            {"upper-centre",          W * 3 / 8,  H / 3,     W / 4, H / 8},
            {"top        (farthest)", W * 3 / 8,  H / 6,     W / 4, H / 8},
        };
    }
    std::printf("%-24s %8s %8s %10s\n", "band", "dx", "dy", "edge-SAD");
    for (const Band& band : bands) {
        const Match m = Search(a, b, band.x, band.y, band.w, band.h, 400, 120, 2);
        std::printf("%-24s %+8d %+8d %10.2f  %s%s\n", band.name, m.dx, m.dy, m.sad,
                    m.boundary ? "BOUNDARY HIT -- not a measurement" : "",
                    (!m.boundary && m.dy != 0) ? "dy != 0: not a pure lateral eye move" : "");

        // THE CURVE, NOT THE ARGMIN. An argmin is a number; a curve says
        // whether that number means anything. A sharp well at one dx is a
        // measurement. A shallow dish, a flat line, or several minima of equal
        // depth are not -- and all three come back from this function looking
        // exactly like the first one if only the argmin is printed.
        //
        // Printed at the best dy, so the horizontal profile is read at the
        // vertical offset that actually matched.
        double at0 = 0.0, best = 1e30, worst = 0.0;
        std::printf("      dx:");
        for (int dx = -40; dx <= 40; dx += 8) std::printf(" %+5d", dx);
        std::printf("\n     SAD:");
        for (int dx = -40; dx <= 40; dx += 8) {
            double sum = 0.0; long long n = 0;
            for (int y = band.y; y < band.y + band.h; y += 3) {
                const int y2 = y + m.dy;
                if (y < 4 || y >= a.height - 4 || y2 < 4 || y2 >= b.height - 4) continue;
                for (int x = band.x; x < band.x + band.w; x += 3) {
                    const int x2 = x + dx;
                    if (x < 4 || x >= a.width - 4 || x2 < 4 || x2 >= b.width - 4) continue;
                    sum += std::abs(Grad(a, x, y) - Grad(b, x2, y2));
                    ++n;
                }
            }
            const double v = n ? sum / static_cast<double>(n) : 0.0;
            if (dx == 0) at0 = v;
            if (v < best) best = v;
            if (v > worst) worst = v;
            std::printf(" %5.1f", v);
        }
        // THREE OUTCOMES, AND THE FIRST BUILD OF THIS COULD ONLY SAY ONE.
        //
        // It printed "well depth vs dx=0" and called anything under 8% SHALLOW
        // -- which mislabels the most important case: when dx=0 IS the sharp
        // minimum, the improvement over dx=0 is zero by definition, so a pair
        // of images that are cleanly UNSHIFTED came out tagged "not a
        // measurement". It is the opposite: it is the cleanest measurement
        // available, and it says the camera did not move.
        const double contrast = (at0 - best) / (worst > 0.0 ? worst : 1.0);
        const double zeroWell = (worst - at0) / (worst > 0.0 ? worst : 1.0);
        std::printf("\n     ");
        if (zeroWell > 0.5) {
            std::printf("SHARP MINIMUM AT dx=0 (%.0f%% below the rest of the curve): the images "
                        "are UNSHIFTED.\n\n", zeroWell * 100.0);
        } else if (contrast < 0.08) {
            std::printf("FLAT: no well anywhere. The argmin above is noise, not a shift.\n\n");
        } else {
            std::printf("well at dx=%+d is %.0f%% below dx=0, over a curve range of %.1f. A real "
                        "shift wants a deep, single well.\n\n", m.dx, contrast * 100.0, worst - best);
        }
    }
    std::printf("\nA lateral eye translation gives dy = 0 everywhere and |dx| that FALLS with\n"
                "distance. Equal dx at every depth is a pan, not parallax.\n");
    return 0;
}
