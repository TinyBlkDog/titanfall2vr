#pragma once
#include <cstdint>

// FC500+0x1B5 tests layer+0x29 AFTER the widget returns, clears it and
// returns false so FC7A0 does not submit the layer's scratch. Never skip
// the widget with an unfilled scratch and then let FC500 return true.
inline bool RuiDeclineCompletedLayer(void* layer, std::uintptr_t original,
                                    std::uintptr_t selected) {
    if (!layer || !selected || original != selected) return false;
    *(static_cast<std::uint8_t*>(layer) + 0x29) = 1;
    return true;
}
