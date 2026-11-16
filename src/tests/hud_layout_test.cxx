#include "../platform/HudLayout.hxx"
#include "../platform/HudTextures.hxx"
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

    // Textures: a sheet's DDS decodes to RGBA and goes out as a PNG.
    std::vector<std::string> dds;
    CHECK(hud_textures::Entries(hud_layout::Pack(hud_fixture::OneSheet()), dds) && dds.size() == 1);
    hud_textures::Image image;
    CHECK(hud_textures::Decode(dds[0], image) && image.width == 4 && image.height == 4);
    CHECK(image.rgba[0] == 255 && image.rgba[1] == 128 && image.rgba[2] == 0 && image.rgba[3] == 255);   // BGRA in, RGBA out
    CHECK(hud_textures::EncodePng(image).compare(1, 3, "PNG") == 0);
    CHECK(!hud_textures::Decode("DDS short", image));
    // PNG back to pixels, and a picture pasted into the sheet (raw BGRA here).
    hud_textures::Image fromPng;
    CHECK(hud_textures::DecodePng(hud_textures::EncodePng(image), 4, 4, fromPng) && fromPng.width == 4 && fromPng.rgba == image.rgba);
    CHECK(!hud_textures::DecodePng(hud_textures::EncodePng(image), 3, 4, fromPng) && !hud_textures::DecodePng(hud_textures::EncodePng(image), 4, 3, fromPng));   // past the most asked
    hud_textures::Image two; two.width = 2; two.height = 2; two.rgba.assign(16, 0);
    for (int i = 0; i < 4; ++i) { two.rgba[4 * i] = 10; two.rgba[4 * i + 1] = 20; two.rgba[4 * i + 2] = 30; two.rgba[4 * i + 3] = 200; }
    std::string pasted;
    CHECK(hud_textures::Paste(hud_layout::Pack(hud_fixture::OneSheet()), 0, two, 1, 1, pasted));
    CHECK(hud_textures::Entries(pasted, dds) && hud_textures::Decode(dds[0], image));
    CHECK(image.rgba[(1 * 4 + 1) * 4] == 10 && image.rgba[(2 * 4 + 2) * 4 + 3] == 200 && image.rgba[0] == 255);   // inside the picture, and the rest untouched
    CHECK(!hud_textures::Paste(hud_layout::Pack(hud_fixture::OneSheet()), 0, two, 3, 3, pasted));   // does not fit
    CHECK(!hud_textures::DecodePng("not a png at all, long enough to pass the size check......", 4096, 4096, fromPng));
    {
        // A small PNG whose header claims 4096 x 4096 is refused on its header, before anything is unpacked.
        std::string huge = hud_textures::EncodePng(image);
        const char side[4] = { 0, 0, 0x10, 0 };
        huge.replace(16, 4, side, 4); huge.replace(20, 4, side, 4);
        CHECK(!hud_textures::DecodePng(huge, 1024, 1024, fromPng));
        // A chunk before the header, and a chunk whose length runs past the end, are refused.
        std::string first = hud_textures::EncodePng(image);
        first.replace(12, 4, "IDAT");
        CHECK(!hud_textures::DecodePng(first, 4, 4, fromPng));
        std::string longChunk = hud_textures::EncodePng(image);
        const char most[4] = { (char)0xFF, (char)0xFF, (char)0xFF, (char)0xF8 };
        longChunk.replace(8, 4, most, 4);
        CHECK(!hud_textures::DecodePng(longChunk, 4, 4, fromPng));
    }
    {
        // Offsets near 0xFFFFFFFF in a sheet's #EMB and a layout's #EMS are refused, not wrapped round to small ones.
        std::string emb(0x40, '\0');
        emb.replace(0, 4, "#EMB");
        const std::uint32_t one = 1, table = 0x20, far = 0xFFFFFFF0u, length = 4;
        std::memcpy(&emb[0xC], &one, 4); std::memcpy(&emb[0x18], &table, 4); std::memcpy(&emb[0x20], &far, 4); std::memcpy(&emb[0x24], &length, 4);
        CHECK(!hud_textures::Entries(emb, dds) && !hud_textures::Paste(hud_layout::Pack(emb), 0, two, 0, 0, pasted));
        std::string bad(0x40, '\0');
        bad.replace(0, 4, "#EMS");
        std::memcpy(&bad[0xC], &one, 4); std::memcpy(&bad[0x14], &table, 4); std::memcpy(&bad[0x20], &far, 4);
        CHECK(hud_layout::Describe({ { "cockpit", bad } }).empty() && hud_layout::Sprites({ { "cockpit", bad } }).empty());
        std::map<std::string, std::string> none;
        CHECK(!hud_layout::Apply({ { "cockpit", bad } }, {}, none));
    }
    // One DXT5 block: alpha 255 throughout, colour 0 pure red (565: 0xF800), every texel colour 0.
    std::string block(128 + 16, '\0');
    block.replace(0, 4, "DDS "); block.replace(84, 4, "DXT5");
    block[12] = 4; block[16] = 4; block[128] = (char)255; block[129] = (char)255; block[136] = 0x00; block[137] = (char)0xF8;
    CHECK(hud_textures::Decode(block, image) && image.rgba[0] == 255 && image.rgba[1] == 0 && image.rgba[2] == 0 && image.rgba[3] == 255);

    // Elements of the files the game reads only as it starts say so (the editor's restart note); the cockpit and the groups do not.
    {
        std::map<std::string, bool> atStart;
        for (const auto& element : hud_layout::Elements()) atStart[element.id] = hud_layout::LoadsAtStart(element.file);
        CHECK(atStart["p1_label"] && atStart["p2_name"] && atStart["p1_face"] && atStart["p2_ucname"] && atStart["kotext"]);
        CHECK(!atStart["p1_life"] && !atStart["clock"] && !atStart["p1_all"]);
    }
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
