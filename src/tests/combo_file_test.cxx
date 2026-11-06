#include "../training/ComboFile.hxx"
#include <cstdio>
#include <stdexcept>
// A saved combo round-trips, and a file from anyone is refused when any field is wrong.
using namespace sf4e::training;
using json = nlohmann::json;
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
SavedCombo Sample() {
    SavedCombo c; c.id = "123-1"; c.name = "Ken 2MK > Hadoken"; c.fighter = 3; c.hits = 4; c.created = 1000; c.x[0] = -1.5f; c.x[1] = 0.25f; c.notation = "2MK > 236P";
    for (int i = 0; i < 5; ++i) c.frames.push_back({0, 0, 0, 0});
    for (int i = 0; i < 3; ++i) c.frames.push_back({0x20, 0x20, 0, 0});
    c.frames.push_back({0x2, 0x2, 0, 0});
    return c;
}
bool Refused(json value, const char* key, json bad) {
    value[key] = bad;
    SavedCombo out; std::string error;
    return !DecodeCombo(value.dump(), out, error) && !error.empty();
}
int main() {
    try {
        const auto sample = Sample();
        SavedCombo out; std::string error;
        const auto text = EncodeCombo(sample);
        Require(DecodeCombo(text, out, error), "A combo did not read back");
        Require(out.id == sample.id && out.name == sample.name && out.fighter == 3 && out.hits == 4 && out.created == 1000 && out.notation == sample.notation &&
            out.x[0] == -1.5f && out.x[1] == 0.25f && out.frames.size() == sample.frames.size(), "A field changed in the round trip");
        for (std::size_t i = 0; i < out.frames.size(); ++i) Require(out.frames[i].mapped == sample.frames[i].mapped && out.frames[i].raw == sample.frames[i].raw, "A frame changed");
        Require(json::parse(text)["frames"].size() == 3, "Equal frames were not written as one run");
        // Bits outside the fight buttons are cleared; a newer version is refused with its own reason.
        auto value = json::parse(text); value["frames"] = json::array({json::array({0xffffffffu, 0xffffffffu, 2})});
        Require(DecodeCombo(value.dump(), out, error) && out.frames.size() == 2 && out.frames[0].raw == FightButtons, "Stray bits were kept");
        value = json::parse(text); value["version"] = ComboVersion + 1;
        Require(!DecodeCombo(value.dump(), out, error) && error == "the combo is from a newer version", "A newer version was read");
        // Hostile input.
        const auto good = json::parse(text);
        Require(Refused(good, "format", "sf4e-recording") && Refused(good, "version", "1") && Refused(good, "id", "../x") && Refused(good, "id", "UPPER") && Refused(good, "id", 5), "A bad format or id was accepted");
        Require(Refused(good, "name", "") && Refused(good, "name", std::string(ComboNameBytes + 1, 'a')) && Refused(good, "name", "a\nb") && Refused(good, "name", 7), "A bad name was accepted");
        Require(Refused(good, "notation", std::string(ComboNotationBytes + 1, 'a')) && Refused(good, "fighter", 1000) && Refused(good, "fighter", "3") && Refused(good, "hits", -1) && Refused(good, "created", -5), "A bad number was accepted");
        Require(Refused(good, "start", json::object()) && Refused(good, "start", json{{"x", json::array({1})}}) && Refused(good, "start", json{{"x", json::array({1, "a"})}}) && Refused(good, "start", json{{"x", json::array({1, 1e30})}}), "A bad start was accepted");
        Require(Refused(good, "frames", json::array()) && Refused(good, "frames", "x") && Refused(good, "frames", json::array({json::array({1, 2})})) && Refused(good, "frames", json::array({json::array({1, 2, 0})})) &&
            Refused(good, "frames", json::array({json::array({1, 2, MaxFrames + 1})})) && Refused(good, "frames", json::array({json::array({1, 2, 1.5})})) && Refused(good, "frames", json::array({json::array({-1, 2, 1})})), "A bad frame run was accepted");
        Require(Refused(good, "frames", json::array({json::array({1, 2, MaxFrames}), json::array({1, 3, 1})})), "A combo past the frame limit was accepted");
        json many = json::array();
        for (int i = 0; i <= MaxFrames; ++i) many.push_back(json::array({i & 1, 0, 1}));
        Require(Refused(good, "frames", many), "Too many runs were accepted");
        Require(!DecodeCombo(std::string(ComboFileBytes + 1, ' '), out, error) && error == "the file is too big", "A big file was read");
        Require(!DecodeCombo(text.substr(0, text.size() / 2), out, error) && !DecodeCombo("", out, error) && !DecodeCombo("[]", out, error) && !DecodeCombo("null", out, error), "A truncated or empty file was read");
        Require(ValidComboId("1730-2") && !ValidComboId("") && !ValidComboId("a/b") && !ValidComboId("a.b") && !ValidComboId(std::string(33, 'a')), "Ids");
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
    return 0;
}
