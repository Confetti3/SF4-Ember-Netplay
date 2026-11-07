#include "../platform/HudLayout.hxx"
#include "test_support.hxx"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

using namespace sf4e::platform;

namespace {
void Put(std::string& d, size_t at, const void* v, size_t n) { std::memcpy(&d[at], v, n); }
void PutU(std::string& d, size_t at, uint32_t v) { Put(d, at, &v, 4); }
void PutF(std::string& d, size_t at, float v) { Put(d, at, &v, 4); }
float GetF(const std::string& d, size_t at) { float v; std::memcpy(&v, d.data() + at, 4); return v; }

// One node "1_vital_base_2" (the P1 plate) at (100, 50), drawn 40x20 around it.
std::string OneNode() {
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

int main() {
    const std::string ems = OneNode();
    std::string back;
    CHECK(hud_layout::Unpack(hud_layout::Pack(ems), back));
    CHECK(back == ems);
    CHECK(!hud_layout::Unpack("#EMZ-too-short", back));

    const std::map<std::string, std::string> files = { { "cockpit", hud_layout::Pack(ems) } };
    const auto info = hud_layout::Describe(files);
    CHECK(info.size() == 2);                       // the plate and the P1 cluster that holds it
    CHECK(info[0].id == "p1_all" && info[1].id == "p1_plate");
    CHECK(info[1].cx == 100 && info[1].cy == 50 && info[1].w == 40 && info[1].h == 20);

    std::map<std::string, std::string> out;
    CHECK(hud_layout::Apply(files, {}, out));
    CHECK(out.empty());                            // nothing edited, nothing written

    hud_layout::Layout layout;
    layout["p1_plate"] = { 300, 200, 0, 1, false };
    CHECK(hud_layout::Apply(files, layout, out));
    CHECK(out.count("cockpit") == 1);
    CHECK(hud_layout::Unpack(out["cockpit"], back));
    CHECK(GetF(back, 0x140) == 300 && GetF(back, 0x144) == 200);

    layout["p1_plate"] = { 100, 50, 0, 1, true };  // hidden: pushed off the canvas
    CHECK(hud_layout::Apply(files, layout, out) && hud_layout::Unpack(out["cockpit"], back));
    CHECK(GetF(back, 0x140) == 4100);

    layout["p1_plate"] = { 100, 50, 0, 2, false }; // twice as big about its centre
    CHECK(hud_layout::Apply(files, layout, out) && hud_layout::Unpack(out["cockpit"], back));
    CHECK(GetF(back, 0xA8) == 40 && GetF(back, 0xAC) == 20);
    return 0;
}
