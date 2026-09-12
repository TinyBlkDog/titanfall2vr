// BUILD: cl /nologo /std:c++20 /EHsc /O2 bmpcrop.cpp /Fe:bmpcrop.exe
//        (from a VS Developer prompt; the .exe is deliberately not committed)

// bmpcrop -- NATIVE-RESOLUTION crops out of the capture BMPs.
//
// The project rule is that the wearer is shown crops at native resolution and
// never a whole-frame downscale, because a downscale destroys exactly the
// evidence being judged: whether an edge has SHIFTED. There was no tool for it,
// so crops were not being sent at all. This is that tool.
//
// It NEVER resamples. Output pixels are input pixels.
//
//   bmpcrop <in.bmp> <out.bmp> <x> <y> <w> <h>
//   bmpcrop <in.bmp> <out.bmp> <x> <y> <w> <h> --scale N    integer NEAREST
//                                                           upscale, N in 1..8
//
// --scale is pixel-doubling, not interpolation: every source pixel becomes an
// NxN block. It magnifies without inventing detail, so a 2 px shift stays a 2 px
// shift and stays countable by eye. Anything that could blur is deliberately
// absent.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma pack(push, 1)
struct FileHeader { std::uint16_t type; std::uint32_t size; std::uint16_t r1, r2; std::uint32_t offset; };
struct InfoHeader { std::uint32_t size; std::int32_t width, height; std::uint16_t planes, bits;
                    std::uint32_t compression, imageSize; std::int32_t xppm, yppm;
                    std::uint32_t used, important; };
#pragma pack(pop)

int Fail(const char* what) { std::fprintf(stderr, "bmpcrop: %s\n", what); return 1; }

int main(int argc, char** argv) {
    if (argc != 7 && argc != 9) {
        std::fprintf(stderr,
            "usage: bmpcrop <in.bmp> <out.bmp> <x> <y> <w> <h> [--scale N]\n"
            "  x,y are from the TOP-LEFT of the image as displayed.\n"
            "  --scale N does an integer nearest-neighbour upscale (N in 1..8).\n");
        return 1;
    }
    int scale = 1;
    if (argc == 9) {
        if (std::strcmp(argv[7], "--scale") != 0) return Fail("expected --scale as argument 7");
        scale = std::atoi(argv[8]);
        if (scale < 1 || scale > 8) return Fail("--scale must be 1..8");
    }

    std::FILE* in = std::fopen(argv[1], "rb");
    if (!in) return Fail("cannot open the input");
    FileHeader fh{}; InfoHeader ih{};
    if (std::fread(&fh, sizeof fh, 1, in) != 1 || std::fread(&ih, sizeof ih, 1, in) != 1) {
        std::fclose(in); return Fail("input is too short to be a BMP");
    }
    if (fh.type != 0x4D42) { std::fclose(in); return Fail("input is not a BMP"); }
    if (ih.compression != 0) { std::fclose(in); return Fail("compressed BMPs are not supported"); }
    if (ih.bits != 32 && ih.bits != 24) { std::fclose(in); return Fail("only 24- and 32-bit BMPs"); }

    // A negative height means the rows are stored TOP-DOWN. Both orders exist in
    // this project's captures, and getting it wrong silently returns a crop of
    // the mirrored part of the frame -- which looks like a real crop of the
    // wrong thing, the worst kind of wrong.
    const bool topDown = ih.height < 0;
    const int srcW = ih.width;
    const int srcH = topDown ? -ih.height : ih.height;
    const int bpp = ih.bits / 8;
    const int srcStride = ((srcW * bpp) + 3) & ~3;

    const int x = std::atoi(argv[3]), y = std::atoi(argv[4]);
    const int w = std::atoi(argv[5]), h = std::atoi(argv[6]);
    if (w <= 0 || h <= 0) return Fail("width and height must be positive");
    if (x < 0 || y < 0 || x + w > srcW || y + h > srcH) {
        std::fclose(in);
        std::fprintf(stderr, "bmpcrop: the crop leaves the image (%dx%d asked at %d,%d of %dx%d)\n",
                     w, h, x, y, srcW, srcH);
        return 1;
    }

    std::vector<unsigned char> src(static_cast<size_t>(srcStride) * srcH);
    std::fseek(in, static_cast<long>(fh.offset), SEEK_SET);
    if (std::fread(src.data(), 1, src.size(), in) != src.size()) {
        std::fclose(in); return Fail("input ended early; the pixel data is short");
    }
    std::fclose(in);

    const int outW = w * scale, outH = h * scale;
    const int outStride = ((outW * bpp) + 3) & ~3;
    std::vector<unsigned char> out(static_cast<size_t>(outStride) * outH, 0);

    for (int row = 0; row < h; ++row) {
        const int srcRow = topDown ? (y + row) : (srcH - 1 - (y + row));
        const unsigned char* s = src.data() + static_cast<size_t>(srcRow) * srcStride + x * bpp;
        for (int rep = 0; rep < scale; ++rep) {
            // Written bottom-up, which is what the header below declares.
            const int outRow = outH - 1 - (row * scale + rep);
            unsigned char* d = out.data() + static_cast<size_t>(outRow) * outStride;
            for (int col = 0; col < w; ++col)
                for (int k = 0; k < scale; ++k)
                    std::memcpy(d + (static_cast<size_t>(col) * scale + k) * bpp, s + col * bpp, bpp);
        }
    }

    FileHeader ofh{}; InfoHeader oih{};
    ofh.type = 0x4D42;
    ofh.offset = sizeof ofh + sizeof oih;
    ofh.size = ofh.offset + static_cast<std::uint32_t>(out.size());
    oih.size = sizeof oih; oih.width = outW; oih.height = outH;
    oih.planes = 1; oih.bits = ih.bits; oih.imageSize = static_cast<std::uint32_t>(out.size());

    std::FILE* o = std::fopen(argv[2], "wb");
    if (!o) return Fail("cannot open the output");
    std::fwrite(&ofh, sizeof ofh, 1, o);
    std::fwrite(&oih, sizeof oih, 1, o);
    std::fwrite(out.data(), 1, out.size(), o);
    std::fclose(o);
    std::printf("bmpcrop: %dx%d at (%d,%d) of %dx%d -> %s (%dx%d, x%d nearest, no resampling)\n",
                w, h, x, y, srcW, srcH, argv[2], outW, outH, scale);
    return 0;
}
