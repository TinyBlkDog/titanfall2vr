// Minimal-but-honest x86-64 decoder. Goal: correct instruction LENGTHS on a
// linear sweep, and readable text for the instruction classes this work needs
// (mov/lea/movss/movups/call/jmp/jcc/test/cmp/add/sub/ret). Anything it does
// not know is printed as a db byte and marked invalid, so a sweep can say so
// rather than silently desynchronising.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace dis {

struct Insn {
    int len = 0;
    std::string text;
    bool ripRelative = false;
    std::int64_t ripTarget = 0;
    bool isCall = false;
    bool isJmp = false;
    bool isCondJmp = false;
    bool isRet = false;
    std::int64_t branchTarget = 0;
    bool hasModrm = false;
    int modrmMod = 0, modrmReg = 0, modrmRm = 0;
    std::int64_t disp = 0;
    bool memOperand = false;
    int memBase = -1;
    bool valid = false;
};

inline const char* Reg64(int r) {
    static const char* n[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                "r8","r9","r10","r11","r12","r13","r14","r15"};
    return n[r & 15];
}
inline const char* Reg32(int r) {
    static const char* n[16] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi",
                                "r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"};
    return n[r & 15];
}
inline const char* Reg16(int r) {
    static const char* n[16] = {"ax","cx","dx","bx","sp","bp","si","di",
                                "r8w","r9w","r10w","r11w","r12w","r13w","r14w","r15w"};
    return n[r & 15];
}
inline const char* Reg8(int r, bool rex) {
    static const char* n[16] = {"al","cl","dl","bl","spl","bpl","sil","dil",
                                "r8b","r9b","r10b","r11b","r12b","r13b","r14b","r15b"};
    static const char* legacy[8] = {"al","cl","dl","bl","ah","ch","dh","bh"};
    if (!rex && r < 8) return legacy[r];
    return n[r & 15];
}
inline std::string Xmm(int r) { return "xmm" + std::to_string(r & 15); }

inline std::string RegName(int r, int size, bool rexPresent) {
    switch (size) {
        case 8: return Reg64(r);
        case 4: return Reg32(r);
        case 2: return Reg16(r);
        case 1: return Reg8(r, rexPresent);
        case 16: return Xmm(r);
        default: return Reg64(r);
    }
}

inline std::string Hex(std::int64_t v) {
    char b[32];
    if (v < 0) std::snprintf(b, sizeof(b), "-0x%llX", (unsigned long long)(-v));
    else std::snprintf(b, sizeof(b), "0x%llX", (unsigned long long)v);
    return b;
}

inline Insn Decode(const std::uint8_t* p, std::size_t avail, std::uint64_t rva) {
    Insn in;
    if (avail == 0) return in;
    std::size_t i = 0;
    bool opsize16 = false, rep = false, repne = false;
    int rex = 0; bool rexPresent = false;
    std::string segPrefix;

    for (;;) {
        if (i >= avail) return in;
        std::uint8_t b = p[i];
        if (b == 0x66) { opsize16 = true; ++i; continue; }
        if (b == 0x67) { ++i; continue; }
        if (b == 0xF0) { ++i; continue; }
        if (b == 0xF3) { rep = true; ++i; continue; }
        if (b == 0xF2) { repne = true; ++i; continue; }
        if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26) { ++i; continue; }
        if (b == 0x64) { segPrefix = "fs:"; ++i; continue; }
        if (b == 0x65) { segPrefix = "gs:"; ++i; continue; }
        break;
    }
    if (i < avail && (p[i] & 0xF0) == 0x40) { rex = p[i] & 0x0F; rexPresent = true; ++i; }
    const bool W = (rex & 8) != 0, R = (rex & 4) != 0, X = (rex & 2) != 0, B = (rex & 1) != 0;

    if (i >= avail) return in;
    std::uint8_t op = p[i++];
    int opLen = 1;
    std::uint8_t op2 = 0, op3 = 0;
    if (op == 0x0F) {
        if (i >= avail) return in;
        op2 = p[i++]; opLen = 2;
        if (op2 == 0x38 || op2 == 0x3A) { if (i >= avail) return in; op3 = p[i++]; opLen = 3; }
    }

    std::string memText;
    bool haveModrm = false;
    int mod = 0, reg = 0, rm = 0, memBase = -1;
    std::int64_t dispVal = 0;
    bool isMem = false, ripRel = false;

    auto readModrm = [&]() -> bool {
        if (i >= avail) { i = avail + 1; return false; }
        std::uint8_t m = p[i++];
        haveModrm = true;
        mod = m >> 6; reg = ((m >> 3) & 7) | (R ? 8 : 0); rm = (m & 7);
        if (mod == 3) { rm |= (B ? 8 : 0); isMem = false; return true; }
        isMem = true;
        int base = rm | (B ? 8 : 0);
        int index = -1, scale = 1;
        bool noBase = false;
        if ((m & 7) == 4) {
            if (i >= avail) { i = avail + 1; return false; }
            std::uint8_t sib = p[i++];
            scale = 1 << (sib >> 6);
            int idx = ((sib >> 3) & 7) | (X ? 8 : 0);
            base = (sib & 7) | (B ? 8 : 0);
            if (idx != 4) index = idx;
            if ((sib & 7) == 5 && mod == 0) noBase = true;
        } else if ((m & 7) == 5 && mod == 0) {
            ripRel = true;
        }
        if (mod == 1) { if (i >= avail) { i = avail + 1; return false; } dispVal = (std::int8_t)p[i++]; }
        else if (mod == 2 || ripRel || (noBase && mod == 0)) {
            if (i + 4 > avail) { i = avail + 1; return false; }
            std::int32_t d; std::memcpy(&d, p + i, 4); i += 4; dispVal = d;
        }
        char buf[64];
        if (ripRel) {
            memText = "[rip+" + Hex(dispVal) + "]";
        } else {
            memBase = noBase ? -1 : base;
            std::string s = "[";
            if (!noBase) s += Reg64(base);
            if (index >= 0) { if (!noBase) s += "+"; s += Reg64(index); if (scale > 1) { std::snprintf(buf, sizeof(buf), "*%d", scale); s += buf; } }
            if (dispVal || (noBase && index < 0)) { if (dispVal >= 0 && (!noBase || index >= 0)) s += "+"; s += Hex(dispVal); }
            s += "]";
            memText = segPrefix + s;
        }
        return true;
    };

    auto imm = [&](int n) -> std::int64_t {
        std::int64_t v = 0;
        if (i + (std::size_t)n > avail) { i = avail + 1; return 0; }
        if (n == 1) v = (std::int8_t)p[i];
        else if (n == 2) { std::int16_t t; std::memcpy(&t, p + i, 2); v = t; }
        else if (n == 4) { std::int32_t t; std::memcpy(&t, p + i, 4); v = t; }
        else if (n == 8) { std::int64_t t; std::memcpy(&t, p + i, 8); v = t; }
        i += n;
        return v;
    };

    int opndSize = W ? 8 : (opsize16 ? 2 : 4);
    std::string text;
    auto rmText = [&](int size) -> std::string {
        return isMem ? memText : RegName(rm, size, rexPresent);
    };
    auto arith = [&](const char* mn, int dir, int size) {
        if (!readModrm()) return;
        std::string r = RegName(reg, size, rexPresent);
        text = std::string(mn) + " " + (dir ? r + ", " + rmText(size) : rmText(size) + ", " + r);
    };

    static const char* grp1[8] = {"add","or","adc","sbb","and","sub","xor","cmp"};

    if (opLen == 1) {
        if (op < 0x40 && (op & 7) <= 5) {
            const char* mn = grp1[(op >> 3) & 7];
            switch (op & 7) {
                case 0: arith(mn, 0, 1); break;
                case 1: arith(mn, 0, opndSize); break;
                case 2: arith(mn, 1, 1); break;
                case 3: arith(mn, 1, opndSize); break;
                case 4: { std::int64_t v = imm(1); text = std::string(mn) + " al, " + Hex(v); } break;
                case 5: { std::int64_t v = imm(opsize16 ? 2 : 4); text = std::string(mn) + " " + RegName(0, opndSize, rexPresent) + ", " + Hex(v); } break;
            }
        } else if (op >= 0x50 && op <= 0x57) {
            text = std::string("push ") + Reg64((op - 0x50) | (B ? 8 : 0));
        } else if (op >= 0x58 && op <= 0x5F) {
            text = std::string("pop ") + Reg64((op - 0x58) | (B ? 8 : 0));
        } else if (op == 0x63) {
            if (readModrm()) text = "movsxd " + RegName(reg, opndSize, rexPresent) + ", " + rmText(4);
        } else if (op == 0x68) { std::int64_t v = imm(4); text = "push " + Hex(v); }
        else if (op == 0x6A) { std::int64_t v = imm(1); text = "push " + Hex(v); }
        else if (op == 0x69) { if (readModrm()) { std::int64_t v = imm(4); text = "imul " + RegName(reg, opndSize, rexPresent) + ", " + rmText(opndSize) + ", " + Hex(v); } }
        else if (op == 0x6B) { if (readModrm()) { std::int64_t v = imm(1); text = "imul " + RegName(reg, opndSize, rexPresent) + ", " + rmText(opndSize) + ", " + Hex(v); } }
        else if (op >= 0x70 && op <= 0x7F) {
            static const char* cc[16] = {"jo","jno","jb","jae","je","jne","jbe","ja",
                                         "js","jns","jp","jnp","jl","jge","jle","jg"};
            std::int64_t v = imm(1);
            in.isCondJmp = true; in.branchTarget = (std::int64_t)rva + (std::int64_t)i + v;
            text = std::string(cc[op & 15]) + " " + Hex(in.branchTarget);
        } else if (op == 0x80) { if (readModrm()) { std::int64_t v = imm(1); text = std::string(grp1[reg & 7]) + " byte " + rmText(1) + ", " + Hex(v); } }
        else if (op == 0x81) { if (readModrm()) { std::int64_t v = imm(opsize16 ? 2 : 4); text = std::string(grp1[reg & 7]) + " " + std::string(isMem ? (W ? "qword " : "dword ") : "") + rmText(opndSize) + ", " + Hex(v); } }
        else if (op == 0x83) { if (readModrm()) { std::int64_t v = imm(1); text = std::string(grp1[reg & 7]) + " " + std::string(isMem ? (W ? "qword " : "dword ") : "") + rmText(opndSize) + ", " + Hex(v); } }
        else if (op == 0x84) arith("test", 0, 1);
        else if (op == 0x85) arith("test", 0, opndSize);
        else if (op == 0x86) arith("xchg", 0, 1);
        else if (op == 0x87) arith("xchg", 0, opndSize);
        else if (op == 0x88) arith("mov", 0, 1);
        else if (op == 0x89) arith("mov", 0, opndSize);
        else if (op == 0x8A) arith("mov", 1, 1);
        else if (op == 0x8B) arith("mov", 1, opndSize);
        else if (op == 0x8D) { if (readModrm()) text = "lea " + RegName(reg, opndSize, rexPresent) + ", " + memText; }
        else if (op == 0x90) text = rep ? "pause" : "nop";
        else if (op == 0x98) text = W ? "cdqe" : "cwde";
        else if (op == 0x99) text = W ? "cqo" : "cdq";
        else if (op == 0x9C) text = "pushfq";
        else if (op == 0x9D) text = "popfq";
        else if (op >= 0xB0 && op <= 0xB7) { std::int64_t v = imm(1); text = "mov " + RegName((op - 0xB0) | (B ? 8 : 0), 1, rexPresent) + ", " + Hex(v); }
        else if (op >= 0xB8 && op <= 0xBF) { std::int64_t v = imm(W ? 8 : (opsize16 ? 2 : 4)); text = "mov " + RegName((op - 0xB8) | (B ? 8 : 0), opndSize, rexPresent) + ", " + Hex(v); }
        else if (op == 0xC0 || op == 0xC1) {
            static const char* sh[8] = {"rol","ror","rcl","rcr","shl","shr","sal","sar"};
            if (readModrm()) { std::int64_t v = imm(1); text = std::string(sh[reg & 7]) + " " + rmText(op == 0xC0 ? 1 : opndSize) + ", " + Hex(v); }
        }
        else if (op == 0xC2) { std::int64_t v = imm(2); in.isRet = true; text = "ret " + Hex(v); }
        else if (op == 0xC3) { in.isRet = true; text = "ret"; }
        else if (op == 0xC6) { if (readModrm()) { std::int64_t v = imm(1); text = "mov byte " + rmText(1) + ", " + Hex(v); } }
        else if (op == 0xC7) { if (readModrm()) { std::int64_t v = imm(opsize16 ? 2 : 4); text = "mov " + std::string(isMem ? (W ? "qword " : "dword ") : "") + rmText(opndSize) + ", " + Hex(v); } }
        else if (op == 0xC9) text = "leave";
        else if (op == 0xCC) text = "int3";
        else if (op == 0xA8) { std::int64_t v = imm(1); text = "test al, " + Hex(v); }
        else if (op == 0xA9) { std::int64_t v = imm(opsize16 ? 2 : 4); text = "test " + RegName(0, opndSize, rexPresent) + ", " + Hex(v); }
        else if (op == 0xE3) { std::int64_t v = imm(1); in.isCondJmp = true; in.branchTarget = (std::int64_t)rva + (std::int64_t)i + v; text = "jrcxz " + Hex(in.branchTarget); }
        else if (op == 0xF4) text = "hlt";
        else if (op == 0xF5) text = "cmc";
        else if (op == 0xF8) text = "clc";
        else if (op == 0xF9) text = "stc";
        else if (op == 0xFC) text = "cld";
        else if (op == 0xFD) text = "std";
        else if (op >= 0xD8 && op <= 0xDF) {
            // x87: length is fully determined by the modrm/sib/disp, which is
            // all a linear sweep needs from it.
            if (readModrm()) { char b[16]; std::snprintf(b, sizeof(b), "x87_%02X ", op); text = std::string(b) + (isMem ? memText : "st"); }
        }
        else if (op >= 0xD0 && op <= 0xD3) {
            static const char* sh[8] = {"rol","ror","rcl","rcr","shl","shr","sal","sar"};
            if (readModrm()) text = std::string(sh[reg & 7]) + " " + rmText((op & 1) ? opndSize : 1) + ((op >= 0xD2) ? ", cl" : ", 1");
        }
        else if (op == 0xE8) { std::int64_t v = imm(4); in.isCall = true; in.branchTarget = (std::int64_t)rva + (std::int64_t)i + v; text = "call " + Hex(in.branchTarget); }
        else if (op == 0xE9) { std::int64_t v = imm(4); in.isJmp = true; in.branchTarget = (std::int64_t)rva + (std::int64_t)i + v; text = "jmp " + Hex(in.branchTarget); }
        else if (op == 0xEB) { std::int64_t v = imm(1); in.isJmp = true; in.branchTarget = (std::int64_t)rva + (std::int64_t)i + v; text = "jmp " + Hex(in.branchTarget); }
        else if (op == 0xF6) { if (readModrm()) { if ((reg & 7) <= 1) { std::int64_t v = imm(1); text = "test byte " + rmText(1) + ", " + Hex(v); } else { static const char* g[8]={"test","test","not","neg","mul","imul","div","idiv"}; text = std::string(g[reg&7]) + " byte " + rmText(1); } } }
        else if (op == 0xF7) { if (readModrm()) { if ((reg & 7) <= 1) { std::int64_t v = imm(4); text = "test " + rmText(opndSize) + ", " + Hex(v); } else { static const char* g[8]={"test","test","not","neg","mul","imul","div","idiv"}; text = std::string(g[reg&7]) + " " + rmText(opndSize); } } }
        else if (op == 0xFE) { if (readModrm()) text = std::string((reg & 7) ? "dec byte " : "inc byte ") + rmText(1); }
        else if (op == 0xFF) {
            if (readModrm()) {
                switch (reg & 7) {
                    case 0: text = "inc " + rmText(opndSize); break;
                    case 1: text = "dec " + rmText(opndSize); break;
                    case 2: in.isCall = true; text = "call " + rmText(8); break;
                    case 3: in.isCall = true; text = "callf " + rmText(8); break;
                    case 4: in.isJmp = true; text = "jmp " + rmText(8); break;
                    case 5: in.isJmp = true; text = "jmpf " + rmText(8); break;
                    case 6: text = "push " + rmText(8); break;
                    default: break;
                }
            }
        }
    } else if (opLen == 2) {
        if (op2 == 0x05) text = "syscall";
        else if (op2 == 0x0B) text = "ud2";
        else if (op2 == 0x10 || op2 == 0x11) {
            const char* mn = rep ? "movss" : (repne ? "movsd" : (opsize16 ? "movupd" : "movups"));
            if (readModrm()) {
                std::string r = Xmm(reg);
                std::string m = isMem ? memText : Xmm(rm);
                text = std::string(mn) + " " + (op2 == 0x10 ? r + ", " + m : m + ", " + r);
            }
        }
        else if (op2 == 0x12 || op2 == 0x13) { if (readModrm()) { std::string r = Xmm(reg); std::string m = isMem ? memText : Xmm(rm); const char* mn = opsize16 ? "movlpd" : "movlps"; text = std::string(mn) + " " + (op2 == 0x12 ? r + ", " + m : m + ", " + r); } }
        else if (op2 == 0x14) { if (readModrm()) text = std::string(opsize16 ? "unpcklpd " : "unpcklps ") + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)); }
        else if (op2 == 0x15) { if (readModrm()) text = std::string(opsize16 ? "unpckhpd " : "unpckhps ") + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)); }
        else if (op2 == 0x16 || op2 == 0x17) { if (readModrm()) { std::string r = Xmm(reg); std::string m = isMem ? memText : Xmm(rm); const char* mn = opsize16 ? "movhpd" : "movhps"; text = std::string(mn) + " " + (op2 == 0x16 ? r + ", " + m : m + ", " + r); } }
        else if (op2 == 0x1E) { if (readModrm()) text = "nop " + rmText(opndSize); }
        else if (op2 == 0x1F) { if (readModrm()) text = "nop " + rmText(opndSize); }
        else if (op2 == 0x28 || op2 == 0x29) {
            const char* mn = opsize16 ? "movapd" : "movaps";
            if (readModrm()) { std::string r = Xmm(reg); std::string m = isMem ? memText : Xmm(rm); text = std::string(mn) + " " + (op2 == 0x28 ? r + ", " + m : m + ", " + r); }
        }
        else if (op2 == 0x2A) { if (readModrm()) text = std::string(rep ? "cvtsi2ss " : (repne ? "cvtsi2sd " : "cvtpi2ps ")) + Xmm(reg) + ", " + (isMem ? memText : RegName(rm, opndSize, rexPresent)); }
        else if (op2 == 0x2C || op2 == 0x2D) { if (readModrm()) text = std::string(rep ? (op2 == 0x2C ? "cvttss2si " : "cvtss2si ") : (repne ? (op2 == 0x2C ? "cvttsd2si " : "cvtsd2si ") : "cvtps2pi ")) + RegName(reg, opndSize, rexPresent) + ", " + (isMem ? memText : Xmm(rm)); }
        else if (op2 == 0x2E || op2 == 0x2F) { if (readModrm()) text = std::string(op2 == 0x2E ? (opsize16 ? "ucomisd " : "ucomiss ") : (opsize16 ? "comisd " : "comiss ")) + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)); }
        else if (op2 >= 0x40 && op2 <= 0x4F) {
            static const char* cc[16] = {"cmovo","cmovno","cmovb","cmovae","cmove","cmovne","cmovbe","cmova",
                                         "cmovs","cmovns","cmovp","cmovnp","cmovl","cmovge","cmovle","cmovg"};
            if (readModrm()) text = std::string(cc[op2 & 15]) + " " + RegName(reg, opndSize, rexPresent) + ", " + rmText(opndSize);
        }
        else if (op2 == 0x51) { if (readModrm()) text = std::string(rep ? "sqrtss " : (repne ? "sqrtsd " : "sqrtps ")) + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)); }
        else if (op2 == 0x54) { if (readModrm()) text = std::string(opsize16 ? "andpd " : "andps ") + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)); }
        else if (op2 == 0x55) { if (readModrm()) text = std::string(opsize16 ? "andnpd " : "andnps ") + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)); }
        else if (op2 == 0x56) { if (readModrm()) text = std::string(opsize16 ? "orpd " : "orps ") + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)); }
        else if (op2 == 0x57) { if (readModrm()) text = std::string(opsize16 ? "xorpd " : "xorps ") + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)); }
        else if (op2 == 0x58 || op2 == 0x59 || op2 == 0x5C || op2 == 0x5D || op2 == 0x5E || op2 == 0x5F) {
            const char* base_[8] = {"add","mul","","","sub","min","div","max"};
            std::string mn = std::string(base_[op2 - 0x58]) + (rep ? "ss" : (repne ? "sd" : (opsize16 ? "pd" : "ps")));
            if (readModrm()) text = mn + " " + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm));
        }
        else if (op2 == 0x5A) { if (readModrm()) text = std::string(rep ? "cvtss2sd " : (repne ? "cvtsd2ss " : (opsize16 ? "cvtpd2ps " : "cvtps2pd "))) + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)); }
        else if (op2 == 0x5B) { if (readModrm()) text = std::string("cvtdq2ps ") + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)); }
        else if (op2 == 0x6E) { if (readModrm()) text = std::string(W ? "movq " : "movd ") + Xmm(reg) + ", " + (isMem ? memText : RegName(rm, opndSize, rexPresent)); }
        else if (op2 == 0x6F) { if (readModrm()) text = std::string(rep ? "movdqu " : "movdqa ") + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)); }
        else if (op2 == 0x70) { if (readModrm()) { std::int64_t v = imm(1); text = std::string("pshufd ") + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)) + ", " + Hex(v); } }
        else if (op2 == 0x76) { if (readModrm()) text = std::string("pcmpeqd ") + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)); }
        else if (op2 == 0x7E) { if (readModrm()) { if (rep) text = "movq " + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)); else text = std::string(W ? "movq " : "movd ") + (isMem ? memText : RegName(rm, opndSize, rexPresent)) + ", " + Xmm(reg); } }
        else if (op2 == 0x7F) { if (readModrm()) text = std::string(rep ? "movdqu " : "movdqa ") + (isMem ? memText : Xmm(rm)) + ", " + Xmm(reg); }
        else if (op2 >= 0x80 && op2 <= 0x8F) {
            static const char* cc[16] = {"jo","jno","jb","jae","je","jne","jbe","ja",
                                         "js","jns","jp","jnp","jl","jge","jle","jg"};
            std::int64_t v = imm(4);
            in.isCondJmp = true; in.branchTarget = (std::int64_t)rva + (std::int64_t)i + v;
            text = std::string(cc[op2 & 15]) + " " + Hex(in.branchTarget);
        }
        else if (op2 >= 0x90 && op2 <= 0x9F) {
            static const char* cc[16] = {"seto","setno","setb","setae","sete","setne","setbe","seta",
                                         "sets","setns","setp","setnp","setl","setge","setle","setg"};
            if (readModrm()) text = std::string(cc[op2 & 15]) + " " + rmText(1);
        }
        else if (op2 == 0x31) text = "rdtsc";
        else if (op2 == 0x0D) { if (readModrm()) text = "prefetch " + rmText(opndSize); }
        else if (op2 >= 0x18 && op2 <= 0x1D) { if (readModrm()) text = "hint_nop " + rmText(opndSize); }
        else if (op2 == 0xA2) text = "cpuid";
        else if (op2 == 0xA3) { if (readModrm()) text = "bt " + rmText(opndSize) + ", " + RegName(reg, opndSize, rexPresent); }
        else if (op2 == 0xAB) { if (readModrm()) text = "bts " + rmText(opndSize) + ", " + RegName(reg, opndSize, rexPresent); }
        else if (op2 == 0xB3) { if (readModrm()) text = "btr " + rmText(opndSize) + ", " + RegName(reg, opndSize, rexPresent); }
        else if (op2 == 0xBB) { if (readModrm()) text = "btc " + rmText(opndSize) + ", " + RegName(reg, opndSize, rexPresent); }
        else if (op2 == 0xBA) { static const char* bt[8] = {"?","?","?","?","bt","bts","btr","btc"}; if (readModrm()) { std::int64_t v = imm(1); text = std::string(bt[reg & 7]) + " " + rmText(opndSize) + ", " + Hex(v); } }
        else if (op2 == 0xA4 || op2 == 0xAC) { if (readModrm()) { std::int64_t v = imm(1); text = std::string(op2 == 0xA4 ? "shld " : "shrd ") + rmText(opndSize) + ", " + RegName(reg, opndSize, rexPresent) + ", " + Hex(v); } }
        else if (op2 == 0xA5 || op2 == 0xAD) { if (readModrm()) text = std::string(op2 == 0xA5 ? "shld " : "shrd ") + rmText(opndSize) + ", " + RegName(reg, opndSize, rexPresent) + ", cl"; }
        else if (op2 == 0xAE) { if (readModrm()) { if (isMem) text = "fxsave/stmxcsr " + memText; else text = rep ? "fence/incssp" : "fence"; } }
        else if (op2 == 0xB0 || op2 == 0xB1) { if (readModrm()) text = "cmpxchg " + rmText(op2 == 0xB0 ? 1 : opndSize) + ", " + RegName(reg, op2 == 0xB0 ? 1 : opndSize, rexPresent); }
        else if (op2 == 0xC0 || op2 == 0xC1) { if (readModrm()) text = "xadd " + rmText(op2 == 0xC0 ? 1 : opndSize) + ", " + RegName(reg, op2 == 0xC0 ? 1 : opndSize, rexPresent); }
        else if (op2 == 0xC2) { if (readModrm()) { std::int64_t v = imm(1); text = std::string(rep ? "cmpss " : (repne ? "cmpsd " : "cmpps ")) + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)) + ", " + Hex(v); } }
        else if (op2 == 0xC7) { if (readModrm()) text = "cmpxchg16b " + rmText(8); }
        else if (op2 >= 0xC8 && op2 <= 0xCF) text = std::string("bswap ") + RegName((op2 - 0xC8) | (B ? 8 : 0), opndSize, rexPresent);
        else if (op2 == 0xBC) { if (readModrm()) text = std::string(rep ? "tzcnt " : "bsf ") + RegName(reg, opndSize, rexPresent) + ", " + rmText(opndSize); }
        else if (op2 == 0xBD) { if (readModrm()) text = std::string(rep ? "lzcnt " : "bsr ") + RegName(reg, opndSize, rexPresent) + ", " + rmText(opndSize); }
        else if (op2 == 0xD0 || op2 == 0xD4 || op2 == 0xD5 || op2 == 0xDB || op2 == 0xE6 || op2 == 0xEB || op2 == 0xF4 || op2 == 0xFE || op2 == 0xFA || op2 == 0xD3 || op2 == 0xD2 || op2 == 0xF2 || op2 == 0xF3 || op2 == 0x62 || op2 == 0x60 || op2 == 0x61 || op2 == 0x68 || op2 == 0x69 || op2 == 0x6A || op2 == 0x6B || op2 == 0x6C || op2 == 0x6D || op2 == 0x64 || op2 == 0x65 || op2 == 0x66 || op2 == 0x74 || op2 == 0x75) {
            char b[16]; std::snprintf(b, sizeof(b), "sse_0F%02X ", op2);
            if (readModrm()) text = std::string(b) + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm));
        }
        else if (op2 == 0xAF) { if (readModrm()) text = "imul " + RegName(reg, opndSize, rexPresent) + ", " + rmText(opndSize); }
        else if (op2 == 0xB6) { if (readModrm()) text = "movzx " + RegName(reg, opndSize, rexPresent) + ", byte " + rmText(1); }
        else if (op2 == 0xB7) { if (readModrm()) text = "movzx " + RegName(reg, opndSize, rexPresent) + ", word " + rmText(2); }
        else if (op2 == 0xBE) { if (readModrm()) text = "movsx " + RegName(reg, opndSize, rexPresent) + ", byte " + rmText(1); }
        else if (op2 == 0xBF) { if (readModrm()) text = "movsx " + RegName(reg, opndSize, rexPresent) + ", word " + rmText(2); }
        else if (op2 == 0xC6) { if (readModrm()) { std::int64_t v = imm(1); text = std::string(opsize16 ? "shufpd " : "shufps ") + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)) + ", " + Hex(v); } }
        else if (op2 == 0xD6) { if (readModrm()) text = "movq " + (isMem ? memText : Xmm(rm)) + ", " + Xmm(reg); }
        else if (op2 == 0xEF) { if (readModrm()) text = "pxor " + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm)); }
    } else {
        if (readModrm()) {
            char b[32]; std::snprintf(b, sizeof(b), "0f%02x%02x", op2, op3);
            if (op2 == 0x3A) imm(1);
            text = std::string(b) + " " + Xmm(reg) + ", " + (isMem ? memText : Xmm(rm));
        }
    }

    if (i > avail || text.empty()) {
        char b[32]; std::snprintf(b, sizeof(b), "db 0x%02X", p[0]);
        in.len = 1; in.text = b; in.valid = false;
        return in;
    }

    in.len = (int)i;
    in.valid = true;
    in.hasModrm = haveModrm;
    in.modrmMod = mod; in.modrmReg = reg; in.modrmRm = rm;
    in.disp = dispVal;
    in.memOperand = isMem;
    in.memBase = memBase;
    if (ripRel) {
        in.ripRelative = true;
        in.ripTarget = (std::int64_t)rva + in.len + dispVal;
        std::string want = "[rip+" + Hex(dispVal) + "]";
        std::size_t pos = text.find(want);
        if (pos != std::string::npos) {
            char r[80];
            std::snprintf(r, sizeof(r), "[rip+%s]{%llX}", Hex(dispVal).c_str(),
                          (unsigned long long)in.ripTarget);
            text.replace(pos, want.size(), r);
        }
    }
    in.text = text;
    return in;
}

}  // namespace dis
