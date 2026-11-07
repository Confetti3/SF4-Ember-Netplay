#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// A tiny #EMS file for the HUD editor's tests.
namespace hud_fixture {
inline void Put(std::string& d, size_t at, const void* v, size_t n) { std::memcpy(&d[at], v, n); }
inline void PutU(std::string& d, size_t at, uint32_t v) { Put(d, at, &v, 4); }
inline void PutF(std::string& d, size_t at, float v) { Put(d, at, &v, 4); }
inline float GetF(const std::string& d, size_t at) { float v; std::memcpy(&v, d.data() + at, 4); return v; }

// One node "1_vital_base_2" (the P1 plate) at (100, 50), drawn 40x20 around it.
inline std::string OneNode() {
    std::string d(0x160, '\0');
    d.replace(0, 4, "#EMS");
    PutU(d, 0xC, 1); PutU(d, 0x14, 0x20);
    PutU(d, 0x20, 0x40);                           // node table
    PutU(d, 0x40 + 0x10, 0x80);                    // node: action
    d.replace(0x40 + 0x20, 14, "1_vital_base_2");
    PutU(d, 0x80 + 4, 0x100);                      // action: G1
    PutF(d, 0xA0, -20); PutF(d, 0xA4, -10); PutF(d, 0xA8, 20); PutF(d, 0xAC, 10);
    int16_t one = 1; Put(d, 0x100 + 8, &one, 2);   // G1: one position key
    PutU(d, 0x100 + 0x1c, 0x140);
    PutF(d, 0x140, 100); PutF(d, 0x144, 50);
    return d;
}
}
