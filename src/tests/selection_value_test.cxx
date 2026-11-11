#include "../Dimps/Dimps__GameEvents.hxx"
#include "../common/SelectionPick.hxx"
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "test_support.hxx"

int main() {
    using Pick = sf4e::wire::ConfirmedCharaConditions;
    using nlohmann::json;
    Pick original{0, 0, 0, 0, 0, 0, 0, 0, 14};
    const json valid = original;
    CHECK(valid.get<Pick>().unc_edition == 14);
    for (const char* field : {"charaID", "costume", "color", "_unused", "personalAction",
                              "winQuote", "ultraCombo", "handicap", "unc_edition"}) {
        for (const json invalid : {json(-1), json(256), json(1000000), json(UINT64_MAX),
                                   json(1.5), json(1.0), json("1"), json(true), json(nullptr), json::array()}) {
            auto input = valid; input[field] = invalid;
            Pick result = original;
            bool rejected = false;
            try { input.get_to(result); } catch (const json::exception&) { rejected = true; }
            CHECK(rejected);
            CHECK(std::memcmp(&result, &original, sizeof(Pick)) == 0);
            CHECK(sf4e::selection::PreferenceByte(input, field, 7) == 7);
        }
        auto missing = valid; missing.erase(field);
        CHECK(sf4e::selection::PreferenceByte(missing, field, 7) == 7);
        bool rejected = false;
        try { missing.get<Pick>(); } catch (const json::exception&) { rejected = true; }
        CHECK(rejected);
    }
    // Byte representation bounds are separate from the catalog's gameplay bounds.
    CHECK(sf4e::selection::ReadByte(json(0)) == 0);
    CHECK(sf4e::selection::ReadByte(json(255)) == 255);
    // A pick's size rides in the wire pick's spare byte and nowhere else: the
    // struct and its other fields are as they were. 1 is tiny; any other byte
    // (SF4's padding) reads as normal, and a normal pick writes 0.
    {
        static_assert(sizeof(Pick) == 9, "the wire pick keeps its size");
        sf4e::selection::Pick pick;
        pick.fighter = 3; pick.costume = 2; pick.size = 1;
        Pick native{};
        sf4e::selection::ToNative(pick, native);
        CHECK(native._unused == 1 && native.charaID == 3 && native.costume == 2);
        const json wire = native;
        CHECK(wire.at("_unused") == 1);
        CHECK(sf4e::selection::FromNative(wire.get<Pick>()).size == 1);
        native._unused = 7;
        CHECK(sf4e::selection::FromNative(native).size == 0);
        pick.size = 0;
        sf4e::selection::ToNative(pick, native);
        CHECK(native._unused == 0 && sf4e::selection::FromNative(native).size == 0);
    }
    std::cout << "Selection JSON rejects malformed values without narrowing or partial writes\n";
}
