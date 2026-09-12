// pescan -- offline analysis of a PE on disk. No game, no session.
//
// Commands (all addresses are RVAs, hex, with or without 0x):
//   sections  <pe>
//   pdata     <pe> <rva>            .pdata entry containing rva (+ chain)
//   xref      <pe> <rva>            rip-relative refs to rva, with owning func
//   callers   <pe> <rva>            E8/E9 rel32 refs to rva, with owning func
//   vtxref    <pe> <rva>            absolute-qword (image-based) refs to rva
//   dis       <pe> <rva> [count]    linear disassembly
//   func      <pe> <rva>            disassemble the whole .pdata function
//   dump      <pe> <rva> <len>      hex dump
//   str       <pe> <text>           find an ASCII string, report its rva
//   strxref   <pe> <text>           find string then rip-refs to it
//   fieldscan <pe> <fnRva> <off>    stores/loads with disp == off inside fn
//   cvarlive  <pe> <name>           is a ConVar read by anything, or dead?
//   exports   <pe> [substr]          named exports (decorated), forwarders marked
//   imports   <pe> [substr]          imported symbols with their IAT slot rva
#include "disasm.h"
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>


struct Pe {
    std::vector<std::uint8_t> file;
    IMAGE_NT_HEADERS64* nt = nullptr;
    IMAGE_SECTION_HEADER* sec = nullptr;
    int nsec = 0;
    std::uint64_t imageBase = 0;

    bool Load(const char* path) {
        FILE* f = std::fopen(path, "rb");
        if (!f) { std::printf("cannot open %s\n", path); return false; }
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        file.resize((std::size_t)n);
        std::fread(file.data(), 1, (std::size_t)n, f);
        std::fclose(f);
        auto* dos = (IMAGE_DOS_HEADER*)file.data();
        nt = (IMAGE_NT_HEADERS64*)(file.data() + dos->e_lfanew);
        sec = IMAGE_FIRST_SECTION(nt);
        nsec = nt->FileHeader.NumberOfSections;
        imageBase = nt->OptionalHeader.ImageBase;
        return true;
    }
    // rva -> pointer into the on-disk image, or nullptr
    const std::uint8_t* At(std::uint64_t rva, std::size_t* availOut = nullptr) const {
        for (int i = 0; i < nsec; ++i) {
            std::uint64_t va = sec[i].VirtualAddress;
            std::uint64_t vsz = sec[i].Misc.VirtualSize;
            if (rva >= va && rva < va + vsz) {
                std::uint64_t off = rva - va;
                if (off >= sec[i].SizeOfRawData) return nullptr;  // uninitialised tail
                if (availOut) *availOut = (std::size_t)(sec[i].SizeOfRawData - off);
                return file.data() + sec[i].PointerToRawData + off;
            }
        }
        return nullptr;
    }
    const char* SectionOf(std::uint64_t rva) const {
        static char name[16];
        for (int i = 0; i < nsec; ++i) {
            if (rva >= sec[i].VirtualAddress &&
                rva < sec[i].VirtualAddress + sec[i].Misc.VirtualSize) {
                std::memset(name, 0, sizeof(name));
                std::memcpy(name, sec[i].Name, 8);
                return name;
            }
        }
        return "?";
    }
    IMAGE_SECTION_HEADER* Find(const char* n) const {
        for (int i = 0; i < nsec; ++i)
            if (std::strncmp((const char*)sec[i].Name, n, 8) == 0) return &sec[i];
        return nullptr;
    }
};

struct Rf { std::uint32_t begin, end, unwind; };

static std::vector<Rf> Pdata(const Pe& pe) {
    std::vector<Rf> out;
    auto& d = pe.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    std::size_t avail = 0;
    const std::uint8_t* p = pe.At(d.VirtualAddress, &avail);
    if (!p) return out;
    std::size_t n = d.Size / 12;
    for (std::size_t i = 0; i < n; ++i) {
        Rf r;
        std::memcpy(&r, p + i * 12, 12);
        out.push_back(r);
    }
    return out;
}

// Returns index into pdata of the entry containing rva, or -1.
static int PdataIndex(const std::vector<Rf>& t, std::uint64_t rva) {
    int lo = 0, hi = (int)t.size() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (rva < t[mid].begin) hi = mid - 1;
        else if (rva >= t[mid].end) lo = mid + 1;
        else return mid;
    }
    return -1;
}

// Follows UNW_FLAG_CHAININFO to the real function entry. Returns begin rva, or 0.
static std::uint64_t RealFunction(const Pe& pe, const std::vector<Rf>& t, std::uint64_t rva,
                                  std::string* chain) {
    int idx = PdataIndex(t, rva);
    if (idx < 0) return 0;
    Rf r = t[idx];
    char buf[64];
    for (int guard = 0; guard < 16; ++guard) {
        std::snprintf(buf, sizeof(buf), "%llX", (unsigned long long)r.begin);
        if (chain) { if (!chain->empty()) *chain += " <- "; *chain += buf; }
        std::size_t avail = 0;
        const std::uint8_t* u = pe.At(r.unwind, &avail);
        if (!u || avail < 4) return r.begin;
        std::uint8_t verFlags = u[0];
        std::uint8_t flags = verFlags >> 3;
        std::uint8_t codes = u[2];
        if (!(flags & 0x4)) return r.begin;  // no CHAININFO
        std::size_t off = 4 + ((codes + 1) & ~1) * 2;
        if (avail < off + 12) return r.begin;
        Rf next;
        std::memcpy(&next, u + off, 12);
        r = next;
    }
    return r.begin;
}

static std::string Own(const Pe& pe, const std::vector<Rf>& t, std::uint64_t rva) {
    std::string chain;
    std::uint64_t f = RealFunction(pe, t, rva, &chain);
    char b[160];
    if (!f) { std::snprintf(b, sizeof(b), "(no .pdata entry)"); return b; }
    std::snprintf(b, sizeof(b), "fn %llX  [+0x%llX]  chain %s",
                  (unsigned long long)f, (unsigned long long)(rva - f), chain.c_str());
    return b;
}

static void DisRange(const Pe& pe, std::uint64_t rva, int count, std::uint64_t stopRva = 0) {
    std::size_t avail = 0;
    const std::uint8_t* p = pe.At(rva, &avail);
    if (!p) { std::printf("rva %llX not mapped\n", (unsigned long long)rva); return; }
    for (int k = 0; k < count; ++k) {
        if (stopRva && rva >= stopRva) break;
        if (avail == 0) break;
        dis::Insn in = dis::Decode(p, avail, rva);
        if (in.len == 0) break;
        char bytes[64] = {0};
        for (int j = 0; j < in.len && j < 12; ++j)
            std::snprintf(bytes + j * 3, 4, "%02X ", p[j]);
        std::printf("%08llX  %-38s %s%s\n", (unsigned long long)rva, bytes, in.text.c_str(),
                    in.valid ? "" : "   <== UNDECODED");
        p += in.len; avail -= in.len; rva += in.len;
    }
}

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: pescan <cmd> <pe> ...\n"); return 1; }
    std::string cmd = argv[1];
    Pe pe;
    if (!pe.Load(argv[2])) return 1;
    auto pd = Pdata(pe);

    auto Arg = [&](int i) -> std::uint64_t {
        return std::strtoull(argv[i], nullptr, 16);
    };

    if (cmd == "exports") {
        // Named exports, with the forwarder case made explicit. Optional
        // filter argv[3] is a plain substring over the name.
        auto& dd = pe.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dd.VirtualAddress) { std::printf("no export directory\n"); return 0; }
        auto* ed = (const IMAGE_EXPORT_DIRECTORY*)pe.At(dd.VirtualAddress);
        if (!ed) { std::printf("export directory rva %08X not mapped\n", (unsigned)dd.VirtualAddress); return 0; }
        auto* fns   = (const std::uint32_t*)pe.At(ed->AddressOfFunctions);
        auto* names = (const std::uint32_t*)pe.At(ed->AddressOfNames);
        auto* ords  = (const std::uint16_t*)pe.At(ed->AddressOfNameOrdinals);
        if (!fns || !names || !ords) { std::printf("export tables not mapped\n"); return 0; }
        const char* filter = (argc > 3) ? argv[3] : nullptr;
        std::printf("export dir rva %08X size %08X  functions %u  names %u  ordinalBase %u\n",
                    (unsigned)dd.VirtualAddress, (unsigned)dd.Size,
                    (unsigned)ed->NumberOfFunctions, (unsigned)ed->NumberOfNames,
                    (unsigned)ed->Base);
        int hits = 0;
        for (std::uint32_t i = 0; i < ed->NumberOfNames; ++i) {
            const char* nm = (const char*)pe.At(names[i]);
            if (!nm) continue;
            if (filter && !std::strstr(nm, filter)) continue;
            std::uint32_t rva = fns[ords[i]];
            bool fwd = (rva >= dd.VirtualAddress && rva < dd.VirtualAddress + dd.Size);
            if (fwd) {
                const char* to = (const char*)pe.At(rva);
                std::printf("  ord %-5u FORWARD -> %s   | %s\n",
                            (unsigned)(ed->Base + ords[i]), to ? to : "?", nm);
            } else {
                std::printf("  ord %-5u %08X (%s) | %s\n",
                            (unsigned)(ed->Base + ords[i]), (unsigned)rva,
                            pe.SectionOf(rva), nm);
            }
            ++hits;
        }
        std::printf("-- %d export names%s\n", hits, filter ? " matching filter" : "");
        return 0;
    }

    if (cmd == "imports") {
        // Every imported symbol with the IAT slot that holds it, so a
        // `call [rip+..]{SLOT}` in a disassembly can be named. Optional
        // filter argv[3] is a plain substring over module name or symbol.
        auto& dd = pe.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dd.VirtualAddress) { std::printf("no import directory\n"); return 0; }
        const char* filter = (argc > 3) ? argv[3] : nullptr;
        int total = 0;
        for (std::uint32_t k = 0;; ++k) {
            auto* desc = (const IMAGE_IMPORT_DESCRIPTOR*)pe.At(dd.VirtualAddress + k * sizeof(IMAGE_IMPORT_DESCRIPTOR));
            if (!desc || !desc->Name) break;
            const char* mod = (const char*)pe.At(desc->Name);
            if (!mod) break;
            // OriginalFirstThunk is the name table; FirstThunk is the IAT.
            std::uint32_t iltRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
            std::uint32_t iatRva = desc->FirstThunk;
            for (std::uint32_t j = 0;; ++j) {
                auto* ilt = (const std::uint64_t*)pe.At(iltRva + j * 8);
                if (!ilt || !*ilt) break;
                std::uint64_t e = *ilt;
                char nmbuf[512];
                if (e & 0x8000000000000000ull) {
                    std::snprintf(nmbuf, sizeof(nmbuf), "#%u", (unsigned)(e & 0xFFFF));
                } else {
                    const char* nm = (const char*)pe.At((std::uint32_t)e + 2);
                    std::snprintf(nmbuf, sizeof(nmbuf), "%s", nm ? nm : "?");
                }
                if (filter && !std::strstr(nmbuf, filter) && !std::strstr(mod, filter)) continue;
                std::printf("  IAT %08X  %-24s %s\n",
                            (unsigned)(iatRva + j * 8), mod, nmbuf);
                ++total;
            }
        }
        std::printf("-- %d imported symbols%s\n", total, filter ? " matching filter" : "");
        return 0;
    }

    if (cmd == "sections") {
        std::printf("imageBase %llX  sections %d  pdata entries %zu\n",
                    (unsigned long long)pe.imageBase, pe.nsec, pd.size());
        for (int i = 0; i < pe.nsec; ++i) {
            char n[9] = {0}; std::memcpy(n, pe.sec[i].Name, 8);
            std::printf("  %-8s rva %08X vsize %08X raw %08X rawsize %08X chars %08X\n",
                        n, (unsigned)pe.sec[i].VirtualAddress, (unsigned)pe.sec[i].Misc.VirtualSize,
                        (unsigned)pe.sec[i].PointerToRawData, (unsigned)pe.sec[i].SizeOfRawData,
                        (unsigned)pe.sec[i].Characteristics);
        }
        return 0;
    }

    if (cmd == "pdata") {
        std::uint64_t rva = Arg(3);
        std::string chain;
        std::uint64_t f = RealFunction(pe, pd, rva, &chain);
        int idx = PdataIndex(pd, rva);
        if (idx < 0) { std::printf("%llX: NO .pdata entry (not a function body)\n", (unsigned long long)rva); return 0; }
        std::printf("%llX: entry begin=%X end=%X unwind=%X  real function %llX  chain %s\n",
                    (unsigned long long)rva, pd[idx].begin, pd[idx].end, pd[idx].unwind,
                    (unsigned long long)f, chain.c_str());
        return 0;
    }

    if (cmd == "xref") {
        std::uint64_t target = Arg(3);
        int hits = 0;
        for (int s = 0; s < pe.nsec; ++s) {
            if (!(pe.sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const std::uint8_t* base = pe.file.data() + pe.sec[s].PointerToRawData;
            std::uint64_t rvaBase = pe.sec[s].VirtualAddress;
            std::uint32_t size = pe.sec[s].SizeOfRawData;
            for (std::uint32_t i = 0; i + 4 <= size; ++i) {
                std::int32_t d;
                std::memcpy(&d, base + i, 4);
                // The instruction ends somewhere in [i+4, i+4+8]; try plausible
                // ends and confirm by decoding backwards a few bytes.
                for (int tail = 0; tail <= 8; ++tail) {
                    std::uint64_t end = rvaBase + i + 4 + tail;
                    if ((std::uint64_t)((std::int64_t)end + d) != target) continue;
                    // walk back up to 15 bytes and find a decode that lands here
                    for (int back = 1; back <= 15; ++back) {
                        std::uint64_t start = rvaBase + i - back;
                        if (start < rvaBase) break;
                        std::size_t avail = 0;
                        const std::uint8_t* q = pe.At(start, &avail);
                        if (!q) break;
                        dis::Insn in = dis::Decode(q, avail, start);
                        if (!in.valid || !in.ripRelative) continue;
                        if ((std::uint64_t)in.ripTarget != target) continue;
                        if (start + in.len != end) continue;
                        std::printf("%08llX  %-52s | %s\n", (unsigned long long)start,
                                    in.text.c_str(), Own(pe, pd, start).c_str());
                        ++hits;
                        break;
                    }
                    break;
                }
            }
        }
        std::printf("-- %d rip-relative refs to %llX\n", hits, (unsigned long long)target);
        return 0;
    }

    // Every rip-relative reference whose target lands in [lo,hi). One linear
    // sweep of .text, so a whole vtable or struct can be xrefed in one pass.
    if (cmd == "xrefrange") {
        std::uint64_t lo = Arg(3), hi = Arg(4);
        int hits = 0;
        for (int s = 0; s < pe.nsec; ++s) {
            if (!(pe.sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            std::uint64_t rva = pe.sec[s].VirtualAddress;
            std::uint64_t end = rva + pe.sec[s].SizeOfRawData;
            std::size_t avail = 0;
            const std::uint8_t* p = pe.At(rva, &avail);
            if (!p) continue;
            while (rva < end && avail) {
                dis::Insn in = dis::Decode(p, avail, rva);
                if (in.len == 0) break;
                if (in.valid && in.ripRelative && (std::uint64_t)in.ripTarget >= lo &&
                    (std::uint64_t)in.ripTarget < hi) {
                    std::printf("%08llX  %-52s | %s\n", (unsigned long long)rva,
                                in.text.c_str(), Own(pe, pd, rva).c_str());
                    ++hits;
                }
                p += in.len; avail -= in.len; rva += in.len;
            }
        }
        std::printf("-- %d refs into [%llX,%llX)\n", hits, (unsigned long long)lo,
                    (unsigned long long)hi);
        return 0;
    }

    // Indirect virtual calls `call [reg+disp]` with a given disp, i.e. one
    // vtable slot, across the whole image.
    if (cmd == "vcall") {
        std::int64_t want = (std::int64_t)Arg(3);
        int hits = 0;
        for (int s = 0; s < pe.nsec; ++s) {
            if (!(pe.sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            std::uint64_t rva = pe.sec[s].VirtualAddress;
            std::uint64_t end = rva + pe.sec[s].SizeOfRawData;
            std::size_t avail = 0;
            const std::uint8_t* p = pe.At(rva, &avail);
            if (!p) continue;
            while (rva < end && avail) {
                dis::Insn in = dis::Decode(p, avail, rva);
                if (in.len == 0) break;
                if (in.valid && in.isCall && in.memOperand && !in.ripRelative && in.disp == want) {
                    std::printf("%08llX  %-32s | %s\n", (unsigned long long)rva,
                                in.text.c_str(), Own(pe, pd, rva).c_str());
                    ++hits;
                }
                p += in.len; avail -= in.len; rva += in.len;
            }
        }
        std::printf("-- %d indirect calls with disp 0x%llX\n", hits, (unsigned long long)want);
        return 0;
    }

    if (cmd == "callers") {
        std::uint64_t target = Arg(3);
        int hits = 0;
        for (int s = 0; s < pe.nsec; ++s) {
            if (!(pe.sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const std::uint8_t* base = pe.file.data() + pe.sec[s].PointerToRawData;
            std::uint64_t rvaBase = pe.sec[s].VirtualAddress;
            std::uint32_t size = pe.sec[s].SizeOfRawData;
            for (std::uint32_t i = 0; i + 5 <= size; ++i) {
                if (base[i] != 0xE8 && base[i] != 0xE9) continue;
                std::int32_t d;
                std::memcpy(&d, base + i + 1, 4);
                std::uint64_t site = rvaBase + i;
                if ((std::uint64_t)((std::int64_t)site + 5 + d) != target) continue;
                std::printf("%08llX  %s %llX | %s\n", (unsigned long long)site,
                            base[i] == 0xE8 ? "call" : "jmp ", (unsigned long long)target,
                            Own(pe, pd, site).c_str());
                ++hits;
            }
        }
        std::printf("-- %d direct call/jmp refs to %llX\n", hits, (unsigned long long)target);
        return 0;
    }

    if (cmd == "vtxref") {
        std::uint64_t target = Arg(3);
        std::uint64_t abs = pe.imageBase + target;
        int hits = 0;
        for (int s = 0; s < pe.nsec; ++s) {
            const std::uint8_t* base = pe.file.data() + pe.sec[s].PointerToRawData;
            std::uint32_t size = pe.sec[s].SizeOfRawData;
            for (std::uint32_t i = 0; i + 8 <= size; i += 8) {
                std::uint64_t v;
                std::memcpy(&v, base + i, 8);
                if (v != abs) continue;
                std::uint64_t rva = pe.sec[s].VirtualAddress + i;
                std::printf("%08llX  qword -> %llX  (%s)\n", (unsigned long long)rva,
                            (unsigned long long)target, pe.SectionOf(rva));
                ++hits;
            }
        }
        std::printf("-- %d absolute qword refs\n", hits);
        return 0;
    }

    // Sweeps EVERY .pdata function end to end. A decoder that mis-sizes an
    // instruction desynchronises and either hits an undecodable byte or
    // overshoots the function end, so both counts are the falsifier for the
    // decoder itself before any conclusion is drawn from it.
    if (cmd == "selftest") {
        std::size_t fns = 0, bad = 0, overshoot = 0, insns = 0;
        std::vector<std::uint64_t> badFns;
        for (const Rf& r : pd) {
            std::size_t avail = 0;
            const std::uint8_t* p = pe.At(r.begin, &avail);
            if (!p) continue;
            ++fns;
            std::uint64_t rva = r.begin;
            bool ok = true;
            while (rva < r.end && avail) {
                dis::Insn in = dis::Decode(p, avail, rva);
                if (in.len == 0) { ok = false; break; }
                if (!in.valid) { ok = false; ++bad; break; }
                ++insns;
                p += in.len; avail -= in.len; rva += in.len;
            }
            if (ok && rva != r.end) { ++overshoot; ok = false; }
            if (!ok && badFns.size() < 20) badFns.push_back(r.begin);
        }
        std::printf("functions %zu  instructions %zu  undecodable %zu  end-mismatch %zu  (%.4f%% bad)\n",
                    fns, insns, bad, overshoot, 100.0 * (bad + overshoot) / (double)fns);
        std::printf("first bad functions:");
        for (auto f : badFns) std::printf(" %llX", (unsigned long long)f);
        std::printf("\n");
        return 0;
    }

    // Full extent of a real function: the root .pdata entry plus every
    // following fragment that chains back to it. Hooking or scanning anything
    // less reads half a function; hooking a FRAGMENT crashes, which is the
    // trap F1 already paid for.
    if (cmd == "extent" || cmd == "fullfunc" || cmd == "ffieldscan") {
        std::uint64_t rva = Arg(3);
        std::string chain0;
        std::uint64_t root = RealFunction(pe, pd, rva, &chain0);
        if (!root) { std::printf("no .pdata entry for %llX\n", (unsigned long long)rva); return 1; }
        int idx = PdataIndex(pd, root);
        std::uint64_t end = pd[idx].end;
        int frags = 1;
        for (int j = idx + 1; j < (int)pd.size(); ++j) {
            if (pd[j].begin != end) break;
            std::string c;
            if (RealFunction(pe, pd, pd[j].begin, &c) != root) break;
            end = pd[j].end;
            ++frags;
        }
        if (cmd == "extent") {
            std::printf("root %llX  end %llX  size %llu bytes  fragments %d\n",
                        (unsigned long long)root, (unsigned long long)end,
                        (unsigned long long)(end - root), frags);
            return 0;
        }
        if (cmd == "fullfunc") {
            std::printf("; function %llX .. %llX (%llu bytes, %d .pdata fragments)\n",
                        (unsigned long long)root, (unsigned long long)end,
                        (unsigned long long)(end - root), frags);
            DisRange(pe, root, 1000000, end);
            return 0;
        }
        // ffieldscan: every non-rip memory operand with this displacement
        std::int64_t off = (std::int64_t)Arg(4);
        std::size_t avail = 0;
        std::uint64_t cur = root;
        const std::uint8_t* p = pe.At(cur, &avail);
        if (!p) return 1;
        int hits = 0;
        while (cur < end && avail) {
            dis::Insn in = dis::Decode(p, avail, cur);
            if (in.len == 0) break;
            if (in.valid && in.memOperand && !in.ripRelative && in.disp == off) {
                std::printf("%08llX  %s\n", (unsigned long long)cur, in.text.c_str());
                ++hits;
            }
            p += in.len; avail -= in.len; cur += in.len;
        }
        std::printf("-- %d operands with disp 0x%llX in %llX..%llX\n", hits,
                    (unsigned long long)off, (unsigned long long)root, (unsigned long long)end);
        return 0;
    }

    // Every function containing a memory operand at `disp` off a non-stack
    // base register. Stack bases (rsp/rbp) are excluded because [rsp+0x28] is
    // just a frame slot and would drown the signal.
    //
    // vec3scan additionally requires disp, disp+4 and disp+8 off the SAME base
    // within a short window -- i.e. a three-float vector, which is what an
    // angle field actually looks like.
    if (cmd == "dispscan" || cmd == "vec3scan") {
        std::int64_t want = (std::int64_t)Arg(3);
        const bool vec3 = (cmd == "vec3scan");
        std::string lastOwner;
        int hits = 0;
        for (int s = 0; s < pe.nsec; ++s) {
            if (!(pe.sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            std::uint64_t rva = pe.sec[s].VirtualAddress;
            std::uint64_t end = rva + pe.sec[s].SizeOfRawData;
            std::size_t avail = 0;
            const std::uint8_t* p = pe.At(rva, &avail);
            if (!p) continue;
            // small ring of recent (disp, base) pairs for the vec3 test
            struct Rec { std::int64_t d; int base; std::uint64_t at; };
            Rec ring[24]{}; int ri = 0;
            while (rva < end && avail) {
                dis::Insn in = dis::Decode(p, avail, rva);
                if (in.len == 0) break;
                if (in.valid && in.memOperand && !in.ripRelative && in.memBase >= 0 &&
                    in.memBase != 4 && in.memBase != 5) {
                    bool report = false;
                    if (!vec3) {
                        report = (in.disp == want);
                    } else if (in.disp == want + 8 &&
                               (in.text.rfind("movss", 0) == 0 || in.text.rfind("mov ", 0) == 0)) {
                        bool sawA = false, sawB = false;
                        for (const Rec& r : ring) {
                            if (r.base != in.memBase) continue;
                            if (rva - r.at > 64) continue;
                            if (r.d == want) sawA = true;
                            if (r.d == want + 4) sawB = true;
                        }
                        report = sawA && sawB;
                    }
                    if (report) {
                        std::string owner = Own(pe, pd, rva);
                        std::printf("%08llX  %-44s | %s\n", (unsigned long long)rva,
                                    in.text.c_str(), owner.c_str());
                        ++hits;
                    }
                    if (!vec3 || in.text.rfind("movss", 0) == 0 || in.text.rfind("mov ", 0) == 0) {
                        ring[ri % 24] = {in.disp, in.memBase, rva};
                        ++ri;
                    }
                }
                p += in.len; avail -= in.len; rva += in.len;
            }
        }
        std::printf("-- %d hits for disp 0x%llX\n", hits, (unsigned long long)want);
        return 0;
    }

    if (cmd == "dis") {
        std::uint64_t rva = Arg(3);
        int count = argc > 4 ? (int)std::strtol(argv[4], nullptr, 10) : 40;
        DisRange(pe, rva, count);
        return 0;
    }

    if (cmd == "func") {
        std::uint64_t rva = Arg(3);
        int idx = PdataIndex(pd, rva);
        if (idx < 0) { std::printf("no .pdata entry for %llX\n", (unsigned long long)rva); return 1; }
        std::printf("; function %X .. %X (%u bytes)\n", pd[idx].begin, pd[idx].end,
                    pd[idx].end - pd[idx].begin);
        DisRange(pe, pd[idx].begin, 100000, pd[idx].end);
        return 0;
    }

    if (cmd == "dump") {
        std::uint64_t rva = Arg(3);
        std::uint64_t len = Arg(4);
        std::size_t avail = 0;
        const std::uint8_t* p = pe.At(rva, &avail);
        if (!p) { std::printf("not mapped\n"); return 1; }
        if (len > avail) len = avail;
        for (std::uint64_t i = 0; i < len; i += 16) {
            std::printf("%08llX  ", (unsigned long long)(rva + i));
            for (int j = 0; j < 16; ++j) {
                if (i + j < len) std::printf("%02X ", p[i + j]); else std::printf("   ");
            }
            std::printf(" ");
            for (int j = 0; j < 16 && i + j < len; ++j) {
                std::uint8_t c = p[i + j];
                std::printf("%c", (c >= 32 && c < 127) ? c : '.');
            }
            std::printf("\n");
        }
        return 0;
    }


    // ---- cvarlive: is this ConVar actually READ by anything? -------------
    //
    // Found the hard way on 2026-08-20. Seven hudwarp_* cvars and two
    // safe-area cvars all read back correctly when written, and all did
    // nothing, because every one of them is REGISTERED AND NEVER READ --
    // dead features left in the binary. Two runs would have been spent
    // discovering that on screen; this answers it offline in a second.
    //
    // The chain, which is the same for every Source-style ConVar:
    //   1. the name string, which appears exactly once (at registration);
    //   2. the single rip-ref to it -- that is the registration thunk;
    //   3. inside that thunk, `lea rcx, <object>` is the ConVar being
    //      constructed (the `this` pointer of the constructor call);
    //   4. every rip-ref to that OBJECT is a use.
    //
    // A live cvar has references beyond its constructor and its atexit
    // teardown. A dead one has exactly those two. The teardown is recognised
    // by having no .pdata entry -- it is a static destructor thunk, not a
    // real function -- which is also how the constructor's tail jump is
    // written.
    //
    // Reports the count and the reader sites. It does NOT prove liveness:
    // a cvar read through a pointer stashed elsewhere, or looked up by name
    // from another module, would not show here. It proves DEADNESS, which is
    // the direction that saves runs.
    if (cmd == "cvarlive") {
        const char* needle = argv[3];
        std::size_t nl = std::strlen(needle);
        std::uint64_t nameRva = 0;
        for (int s = 0; s < pe.nsec && !nameRva; ++s) {
            const std::uint8_t* base = pe.file.data() + pe.sec[s].PointerToRawData;
            std::uint32_t size = pe.sec[s].SizeOfRawData;
            for (std::uint32_t i = 0; i + nl + 1 <= size; ++i) {
                if (std::memcmp(base + i, needle, nl) != 0) continue;
                if (base[i + nl] != 0) continue;
                if (i > 0 && base[i - 1] != 0) continue;
                nameRva = pe.sec[s].VirtualAddress + i;
                break;
            }
        }
        if (!nameRva) { std::printf("%s: NAME STRING NOT IN THIS MODULE\n", needle); return 1; }

        // The one rip-ref to the name is the registration thunk.
        std::uint64_t reg = 0;
        int nameRefs = 0;
        for (int s = 0; s < pe.nsec; ++s) {
            if (!(pe.sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const std::uint8_t* base = pe.file.data() + pe.sec[s].PointerToRawData;
            std::uint64_t rvaBase = pe.sec[s].VirtualAddress;
            std::uint32_t size = pe.sec[s].SizeOfRawData;
            for (std::uint32_t i = 0; i + 4 <= size; ++i) {
                std::int32_t d; std::memcpy(&d, base + i, 4);
                for (int tail = 0; tail <= 8; ++tail) {
                    std::uint64_t end = rvaBase + i + 4 + tail;
                    if ((std::uint64_t)((std::int64_t)end + d) != nameRva) continue;
                    ++nameRefs;
                    if (!reg) reg = RealFunction(pe, pd, rvaBase + i, nullptr);
                    if (!reg) reg = rvaBase + i;
                    tail = 9;
                }
            }
        }
        if (!reg) { std::printf("%s: name string at %llX has no code reference\n",
                                needle, (unsigned long long)nameRva); return 1; }

        // Inside the thunk, the ConVar object is the `lea rcx, <obj>`.
        std::uint64_t obj = 0;
        {
            int idx = PdataIndex(pd, reg);
            std::uint64_t end = idx >= 0 ? pd[idx].end : reg + 0x100;
            std::uint64_t rva = reg;
            while (rva < end) {
                std::size_t avail = 0;
                const std::uint8_t* p = pe.At(rva, &avail);
                if (!p || !avail) break;
                dis::Insn in = dis::Decode(p, avail, rva);
                if (!in.len) break;
                // REX.W 8D /r with ModRM reg field = 1 (rcx), mod = 00, rm = 101
                if (in.len >= 7 && (p[0] & 0xF8) == 0x48 && p[1] == 0x8D && p[2] == 0x0D) {
                    std::int32_t d; std::memcpy(&d, p + 3, 4);
                    obj = (std::uint64_t)((std::int64_t)(rva + 7) + d);
                    break;
                }
                rva += in.len;
            }
        }
        if (!obj) { std::printf("%s: registration %llX has no 'lea rcx' object\n",
                                needle, (unsigned long long)reg); return 1; }

        std::printf("%s: name %llX  registration %llX  ConVar object %llX\n", needle,
                    (unsigned long long)nameRva, (unsigned long long)reg,
                    (unsigned long long)obj);

        int total = 0, readers = 0;
        for (int s = 0; s < pe.nsec; ++s) {
            if (!(pe.sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const std::uint8_t* base = pe.file.data() + pe.sec[s].PointerToRawData;
            std::uint64_t rvaBase = pe.sec[s].VirtualAddress;
            std::uint32_t size = pe.sec[s].SizeOfRawData;
            for (std::uint32_t i = 0; i + 4 <= size; ++i) {
                std::int32_t d; std::memcpy(&d, base + i, 4);
                for (int tail = 0; tail <= 8; ++tail) {
                    std::uint64_t end = rvaBase + i + 4 + tail;
                    if ((std::uint64_t)((std::int64_t)end + d) != obj) continue;
                    // THE BACK-WALK IS NOT OPTIONAL. Without it this matches any
                    // four bytes that happen to arithmetic to the target, and the
                    // first version reported five imaginary readers for a cvar
                    // whose validated reference count is two.
                    for (int back = 1; back <= 15; ++back) {
                        std::uint64_t start = rvaBase + i - back;
                        if (start < rvaBase) break;
                        std::size_t avail = 0;
                        const std::uint8_t* q = pe.At(start, &avail);
                        if (!q) break;
                        dis::Insn in = dis::Decode(q, avail, start);
                        if (!in.valid || !in.ripRelative) continue;
                        if ((std::uint64_t)in.ripTarget != obj) continue;
                        if (start + in.len != end) continue;
                        std::uint64_t owner = RealFunction(pe, pd, start, nullptr);
                        ++total;
                        if (owner && owner != reg) {
                            ++readers;
                            std::printf("   READER at %08llX  in fn %08llX\n",
                                        (unsigned long long)start, (unsigned long long)owner);
                        }
                        break;
                    }
                    break;
                }
            }
        }
        // WHAT THIS CAN AND CANNOT CONCLUDE.
        //
        // LIVE is a real finding: a direct reader exists and can be read.
        //
        // The absence of one is NOT deadness, and the positive control says so
        // in the strongest possible terms: r_drawviewmodel has exactly this
        // signature -- two refs, no direct reader in client.dll -- and it
        // demonstrably works, because this project ships a feature built on it.
        // A cvar reached through ICvar::FindVar, through a pointer cached at
        // registration, or from another module entirely leaves no rip-relative
        // trace here at all. So the empty result is INCONCLUSIVE and must never
        // be reported as proof that writing the cvar cannot matter.
        std::printf("%s: %d confirmed refs to the object, %d of them READERS -> %s\n", needle,
                    total, readers,
                    readers ? "LIVE in this module"
                            : "NO DIRECT READER IN THIS MODULE -- INCONCLUSIVE, NOT PROOF OF DEAD "
                              "(r_drawviewmodel reads identically and works)");
        return 0;
    }
    if (cmd == "str" || cmd == "strxref") {
        const char* needle = argv[3];
        std::size_t nl = std::strlen(needle);
        std::vector<std::uint64_t> found;
        for (int s = 0; s < pe.nsec; ++s) {
            const std::uint8_t* base = pe.file.data() + pe.sec[s].PointerToRawData;
            std::uint32_t size = pe.sec[s].SizeOfRawData;
            for (std::uint32_t i = 0; i + nl + 1 <= size; ++i) {
                if (std::memcmp(base + i, needle, nl) != 0) continue;
                if (base[i + nl] != 0) continue;             // exact, NUL-terminated
                if (i > 0 && base[i - 1] != 0) continue;     // starts a string
                std::uint64_t rva = pe.sec[s].VirtualAddress + i;
                std::printf("string \"%s\" at %08llX (%s)\n", needle,
                            (unsigned long long)rva, pe.SectionOf(rva));
                found.push_back(rva);
            }
        }
        if (cmd == "str") return 0;
        for (std::uint64_t r : found) {
            std::printf("-- refs to %llX --\n", (unsigned long long)r);
            char buf[32]; std::snprintf(buf, sizeof(buf), "%llX", (unsigned long long)r);
            const char* a[4] = {argv[0], "xref", argv[2], buf};
            char* av[4] = {(char*)a[0], (char*)a[1], (char*)a[2], (char*)a[3]};
            // reuse: recursive call is simplest
            main(4, av);
        }
        return 0;
    }

    if (cmd == "fieldscan") {
        std::uint64_t fn = Arg(3);
        std::int64_t off = (std::int64_t)Arg(4);
        int idx = PdataIndex(pd, fn);
        std::uint64_t end = idx >= 0 ? pd[idx].end : fn + 0x400;
        std::uint64_t rva = idx >= 0 ? pd[idx].begin : fn;
        std::size_t avail = 0;
        const std::uint8_t* p = pe.At(rva, &avail);
        if (!p) return 1;
        while (rva < end && avail) {
            dis::Insn in = dis::Decode(p, avail, rva);
            if (in.len == 0) break;
            if (in.valid && in.memOperand && !in.ripRelative && in.disp == off) {
                std::printf("%08llX  %s\n", (unsigned long long)rva, in.text.c_str());
            }
            p += in.len; avail -= in.len; rva += in.len;
        }
        return 0;
    }

    std::printf("unknown command %s\n", cmd.c_str());
    return 1;
}
