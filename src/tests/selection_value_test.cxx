#include "../Dimps/Dimps__GameEvents.hxx"
#include <cstdlib>
#include <cstring>
#include <iostream>

#define CHECK(c) do { if (!(c)) { std::cerr << "Selection check failed at " << __LINE__ << '\n'; std::exit(1); } } while (false)

int main() {
    using Pick = Dimps::GameEvents::VsMode::ConfirmedCharaConditions;
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
    std::cout << "Selection JSON rejects malformed values without narrowing or partial writes\n";
}
