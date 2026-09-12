#include "../plugin/src/rui_decline.h"
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>

extern "C" unsigned char CallEngineDecline(void* code, void* layer);

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    // Map as data only: no engine initialization, game, headset or imports run.
    const auto image = LoadLibraryExA(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!image) return 3;
    // Exact instruction span independently disassembled with pescan, within
    // FC500's verified .pdata extent. Execute the game's bytes, not a model
    // of its flag consumption. Its surrounding epilogue is replaced by RET.
    const unsigned char expected[] = {
        0x80,0x7B,0x29,0x00,0x74,0x08,0xC6,0x43,0x29,0x00,
        0x32,0xC0,0xEB,0x02,0xB0,0x01
    };
    const auto source = reinterpret_cast<const unsigned char*>(image) + 0xFC6B5;
    if (std::memcmp(source, expected, sizeof(expected))) return 4;
    auto code = static_cast<unsigned char*>(VirtualAlloc(nullptr, sizeof(expected)+1,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!code) return 5;
    std::memcpy(code, source, sizeof(expected));
    code[sizeof(expected)] = 0xC3;
    DWORD old = 0;
    if (!VirtualProtect(code, sizeof(expected)+1, PAGE_EXECUTE_READ, &old)) return 6;
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(expected)+1);
    int checks = 0, failures = 0;
    auto check = [&](bool value, const char* label) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL %s\n", label); }
    };
    std::array<unsigned char, 0x40> layer{};
    // Addresses only establish identity; no game-space constants are used.
    const auto original = reinterpret_cast<std::uintptr_t>(&main);
    const auto other = reinterpret_cast<std::uintptr_t>(&CallEngineDecline);
    check(!RuiDeclineCompletedLayer(nullptr, original, original), "null layer");
    check(!RuiDeclineCompletedLayer(layer.data(), original, 0), "disarmed");
    check(!RuiDeclineCompletedLayer(layer.data(), original, other), "different widget");
    check(CallEngineDecline(code, layer.data()) == 1, "untouched engine accepts");
    check(RuiDeclineCompletedLayer(layer.data(), original, original), "instrument emits");
    check(layer[0x29] == 1, "engine decline requested");
    check(CallEngineDecline(code, layer.data()) == 0, "actual engine bytes decline");
    check(layer[0x29] == 0, "actual engine bytes reset flag");
    check(CallEngineDecline(code, layer.data()) == 1, "next draw restored");
    bool clean = true;
    for (auto byte : layer) clean = clean && byte == 0;
    check(clean, "no adjacent layer writes");
    std::printf("RUI decline: %d/%d passed; actual FC500 decline bytes executed offline.\n",
        checks-failures, checks);
    VirtualFree(code, 0, MEM_RELEASE);
    FreeLibrary(image);
    return failures ? 1 : 0;
}
