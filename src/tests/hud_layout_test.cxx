#include "../platform/HudLayout.hxx"
#include "hud_fixture.hxx"
#include "test_support.hxx"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

using namespace sf4e::platform;

int main() {
    const std::string ems = hud_fixture::OneNode();
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
    CHECK(hud_fixture::GetF(back, 0x140) == 300 && hud_fixture::GetF(back, 0x144) == 200);

    layout["p1_plate"] = { 100, 50, 0, 1, true };  // hidden: pushed off the canvas
    CHECK(hud_layout::Apply(files, layout, out) && hud_layout::Unpack(out["cockpit"], back));
    CHECK(hud_fixture::GetF(back, 0x140) == 4100);

    layout["p1_plate"] = { 100, 50, 0, 2, false }; // twice as big about its centre
    CHECK(hud_layout::Apply(files, layout, out) && hud_layout::Unpack(out["cockpit"], back));
    CHECK(hud_fixture::GetF(back, 0xA8) == 40 && hud_fixture::GetF(back, 0xAC) == 20);

    // layout.json: good entries survive, bad ones and unknown ids do not.
    const auto json = nlohmann::json::parse(R"({"p1_plate":{"x":1,"y":2,"rot":3,"s":0.5,"hide":true},"p2_name":{"x":"no"},"nothing":{"x":1,"y":1,"rot":0,"s":1},"p1_life":{"x":1,"y":2,"rot":0,"s":0}})");
    const auto read = hud_layout::LayoutFromJson(json);
    CHECK(read.size() == 1 && read.at("p1_plate").hide && read.at("p1_plate").scale == 0.5);
    CHECK(hud_layout::LayoutFromJson(hud_layout::LayoutToJson(read)).at("p1_plate").rot == 3);
    CHECK(hud_layout::LayoutFromJson(nlohmann::json::array()).empty());

    CHECK(hud_layout::DesktopAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/120 Safari/537.36"));
    CHECK(hud_layout::DesktopAgent("Mozilla/5.0 (X11; Linux x86_64; rv:120.0) Gecko/20100101 Firefox/120.0"));
    CHECK(!hud_layout::DesktopAgent("Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit Mobile/15E148"));
    CHECK(!hud_layout::DesktopAgent("Mozilla/5.0 (Linux; Android 14; Pixel 8) Chrome/120 Mobile Safari/537.36"));
    CHECK(!hud_layout::DesktopAgent("Mozilla/5.0 (iPad; CPU OS 17_0 like Mac OS X) AppleWebKit"));
    CHECK(!hud_layout::DesktopAgent(""));
    return 0;
}
