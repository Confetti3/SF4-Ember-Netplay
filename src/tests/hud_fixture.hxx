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
    std::string d(0x1A0, '\0');
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
    PutU(d, 0x80 + 8, 0x160);                      // action: G2, whose first key shows texture 0, pixels 0,0 to 4,4
    PutU(d, 0x160 + 0x14, 0x180);
    PutF(d, 0x190, 0); PutF(d, 0x194, 0); PutF(d, 0x198, 4); PutF(d, 0x19C, 4);
    return d;
}

// The #EMB of a texture sheet: one 4x4 DDS, 32 bit BGRA, every pixel orange (to be packed as .tex.emz).
inline std::string OneSheet() {
    std::string dds(128 + 64, '\0');
    dds.replace(0, 4, "DDS ");
    PutU(dds, 12, 4); PutU(dds, 16, 4); PutU(dds, 88, 32);
    for (int i = 0; i < 16; ++i) { dds[128 + 4 * i] = 0; dds[128 + 4 * i + 1] = (char)128; dds[128 + 4 * i + 2] = (char)255; dds[128 + 4 * i + 3] = (char)255; }
    std::string emb(0x30, '\0');
    emb.replace(0, 4, "#EMB");
    PutU(emb, 0xC, 1); PutU(emb, 0x18, 0x20);
    PutU(emb, 0x20, 0x10); PutU(emb, 0x24, (uint32_t)dds.size());      // the entry: 0x10 on from its slot, size
    return emb + dds;
}
}
