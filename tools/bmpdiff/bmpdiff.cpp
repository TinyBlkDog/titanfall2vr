// BUILD: cl /nologo /std:c++20 /EHsc /O2 bmpdiff.cpp /Fe:bmpdiff.exe
//        (from a VS Developer prompt; the .exe is deliberately not committed)

// bmpdiff -- compare two 32-bit BMPs written by DumpBackbuffer.
//
// Built because a visual finding was being carried by a person's memory of a
// 55 ms flash. A per-cell brightness table says WHERE two frames differ and by
// how much, which is what "some sections on the side darker" needs in order to
// become a measurement. Also writes a difference bitmap for looking at.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#pragma pack(push,1)
struct BmpFile { std::uint16_t type; std::uint32_t size; std::uint16_t r1,r2; std::uint32_t off; };
struct BmpInfo { std::uint32_t size; std::int32_t w,h; std::uint16_t planes,bpp;
                 std::uint32_t comp,imgsize; std::int32_t xppm,yppm; std::uint32_t used,important; };
#pragma pack(pop)

static bool Load(const char* p, std::vector<std::uint8_t>& px, int& w, int& h) {
    FILE* f = std::fopen(p, "rb");
    if (!f) { std::printf("cannot open %s\n", p); return false; }
    BmpFile fh{}; BmpInfo ih{};
    std::fread(&fh, sizeof(fh), 1, f);
    std::fread(&ih, sizeof(ih), 1, f);
    if (ih.bpp != 32) { std::printf("%s is %u bpp, expected 32\n", p, ih.bpp); std::fclose(f); return false; }
    w = ih.w; h = ih.h < 0 ? -ih.h : ih.h;
    px.resize((std::size_t)w * h * 4);
    std::fseek(f, fh.off, SEEK_SET);
    std::fread(px.data(), 1, px.size(), f);
    std::fclose(f);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: bmpdiff a.bmp b.bmp [out.bmp]\n"); return 1; }
    std::vector<std::uint8_t> a, b; int aw=0, ah=0, bw=0, bh=0;
    if (!Load(argv[1], a, aw, ah) || !Load(argv[2], b, bw, bh)) return 1;
    if (aw != bw || ah != bh) { std::printf("size mismatch %dx%d vs %dx%d\n", aw, ah, bw, bh); return 1; }

    const int GX = 8, GY = 6;
    double cellA[GY][GX]{}, cellB[GY][GX]{}, cellD[GY][GX]{};
    long long cellN[GY][GX]{};
    double total = 0; long long changed = 0;
    std::vector<std::uint8_t> out(a.size());

    for (int y = 0; y < ah; ++y) {
        for (int x = 0; x < aw; ++x) {
            const std::size_t i = ((std::size_t)y * aw + x) * 4;
            const double la = 0.299*a[i+2] + 0.587*a[i+1] + 0.114*a[i+0];
            const double lb = 0.299*b[i+2] + 0.587*b[i+1] + 0.114*b[i+0];
            const double d = lb - la;
            total += (d < 0 ? -d : d);
            if ((d < 0 ? -d : d) > 8.0) ++changed;
            const int gx = x * GX / aw, gy = y * GY / ah;
            cellA[gy][gx] += la; cellB[gy][gx] += lb; cellD[gy][gx] += d; ++cellN[gy][gx];
            int v = (int)(128.0 + d * 4.0); if (v < 0) v = 0; if (v > 255) v = 255;
            out[i+0] = out[i+1] = out[i+2] = (std::uint8_t)v; out[i+3] = 255;
        }
    }

    std::printf("%dx%d  mean|diff| %.2f  pixels changed >8: %.1f%%\n",
                aw, ah, total / ((double)aw*ah), 100.0 * changed / ((double)aw*ah));
    std::printf("\nper-cell mean brightness  (rows top->bottom as stored; A / B / B-A)\n");
    for (int gy = 0; gy < GY; ++gy) {
        for (int gx = 0; gx < GX; ++gx) {
            const double n = (double)cellN[gy][gx];
            std::printf(" %5.1f/%5.1f/%+6.1f", cellA[gy][gx]/n, cellB[gy][gx]/n, cellD[gy][gx]/n);
        }
        std::printf("\n");
    }

    if (argc > 3) {
        FILE* f = std::fopen(argv[3], "wb");
        if (f) {
            BmpFile fh{}; BmpInfo ih{};
            fh.type = 0x4D42; fh.off = sizeof(fh) + sizeof(ih);
            fh.size = fh.off + (std::uint32_t)out.size();
            ih.size = sizeof(ih); ih.w = aw; ih.h = ah; ih.planes = 1; ih.bpp = 32;
            ih.imgsize = (std::uint32_t)out.size();
            std::fwrite(&fh, sizeof(fh), 1, f); std::fwrite(&ih, sizeof(ih), 1, f);
            std::fwrite(out.data(), 1, out.size(), f); std::fclose(f);
            std::printf("\ndifference bitmap -> %s (grey 128 = identical)\n", argv[3]);
        }
    }
    return 0;
}
