#pragma once

#include <cstdint>

namespace sf4e { namespace input {
// Physical identity is independent of lobby position and fighting-player side.
struct ControllerDevice {
    int type = -1;
    int index = -1;
    bool Valid() const { return (type == 3 || type == 4) && index >= 0 && index < 12; }
};

inline std::uint32_t MapButtons(std::uint32_t physical, const std::uint32_t* bindings) {
    std::uint32_t result = 0;
    for (unsigned bit = 0; bit < 32; ++bit)
        if (physical & (std::uint32_t(1) << bit)) result |= bindings[bit];
    return result;
}
inline std::uint32_t PhysicalBinding(std::uint32_t action,const std::uint32_t* bindings) {
    for(unsigned bit=4;bit<32;++bit)if(bindings[bit]==action)return std::uint32_t(1)<<bit;
    return 0;
}
} }
