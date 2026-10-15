#include "../platform/UiPreferencesStore.hxx"
#include "../common/UpdateChannel.hxx"

#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "test_support.hxx"
#include "temp_root.hxx"

using Path = std::filesystem::path;

static void Write(const Path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc); file << bytes; file.close(); CHECK(file.good());
}
static std::string Read(const Path& path) {
    std::ifstream file(path, std::ios::binary); CHECK(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int main() {
    using namespace sf4e::platform::testing;
    const auto root = MakeTempRoot(L"sf4e-ui-preferences-test-");
    CHECK(std::filesystem::create_directory(root));
    std::string error;
    CHECK(LoadLanguagePreferenceFrom(root.wstring()) == "auto");
    CHECK(!std::filesystem::exists(root / L"ui-preferences.json"));
    CHECK(SaveLanguagePreferenceTo(root.wstring(), "pt-BR", error));
    CHECK(LoadLanguagePreferenceFrom(root.wstring()) == "pt-BR");
    CHECK(Read(root / L"ui-preferences.json").find("\"pt-BR\"") != std::string::npos);
    CHECK(SaveLanguagePreferenceTo(root.wstring(), "auto", error));
    CHECK(LoadLanguagePreferenceFrom(root.wstring()) == "auto");
    CHECK(SaveLanguagePreferenceTo(root.wstring(), "zh-Hans", error));
    CHECK(LoadLanguagePreferenceFrom(root.wstring()) == "zh-Hans");
    CHECK(!SaveLanguagePreferenceTo(root.wstring(), "xx", error));
    CHECK(!SaveLanguagePreferenceTo(root.wstring(), "zh", error));
    // The hidden card and the language share the file without overwriting each other.
    CHECK(!GameSettingsCardHiddenIn(root.wstring()));
    CHECK(SaveLanguagePreferenceTo(root.wstring(), "es-419", error));
    CHECK(HideGameSettingsCardIn(root.wstring(), error));
    CHECK(GameSettingsCardHiddenIn(root.wstring()) && LoadLanguagePreferenceFrom(root.wstring()) == "es-419");
    CHECK(SaveLanguagePreferenceTo(root.wstring(), "auto", error));
    CHECK(GameSettingsCardHiddenIn(root.wstring()));
    // The update channel shares the file the same way, and is unchosen until saved.
    using sf4e::updates::UpdateChannel;
    CHECK(!UpdateChannelPreferenceIn(root.wstring()));
    CHECK(SaveUpdateChannelPreferenceTo(root.wstring(), "prerelease", error) && UpdateChannelPreferenceIn(root.wstring()) == UpdateChannel::Beta);
    CHECK(GameSettingsCardHiddenIn(root.wstring()) && LoadLanguagePreferenceFrom(root.wstring()) == "auto");
    CHECK(SaveUpdateChannelPreferenceTo(root.wstring(), "stable", error) && UpdateChannelPreferenceIn(root.wstring()) == UpdateChannel::Stable);
    CHECK(SaveUpdateChannelPreferenceTo(root.wstring(), "nightly", error) && UpdateChannelPreferenceIn(root.wstring()) == UpdateChannel::Nightly);
    CHECK(!SaveUpdateChannelPreferenceTo(root.wstring(), "bogus", error) && UpdateChannelPreferenceIn(root.wstring()) == UpdateChannel::Nightly);
    for (const auto& info : sf4e::updates::kUpdateChannels) {
        CHECK(SaveUpdateChannelPreferenceTo(root.wstring(), info.stored, error));
        CHECK(UpdateChannelPreferenceIn(root.wstring()) == info.channel);
    }
    for (const char* invalidChannel : {"", "beta", "Nightly", " nightly", "stable "})
        CHECK(!SaveUpdateChannelPreferenceTo(root.wstring(), invalidChannel, error));
    // Unknown and mistyped stored choices remain unchosen rather than becoming Stable.
    const auto unknown = root / L"unknown-channel"; CHECK(std::filesystem::create_directory(unknown));
    for (const char* value : {"\"bogus\"", "\"beta\"", "\"\"", "7", "null"}) {
        Write(unknown / L"ui-preferences.json", std::string("{\"schemaVersion\":1,\"language\":\"de\",\"updateChannel\":") + value + "}");
        CHECK(!UpdateChannelPreferenceIn(unknown.wstring()));
        CHECK(LoadLanguagePreferenceFrom(unknown.wstring()) == "de");
    }
    // A file with no channel field, and one that is not a JSON object, are unchosen too.
    for (const char* body : {"{\"schemaVersion\":1,\"language\":\"de\"}", "[]", "{}"}) {
        Write(unknown / L"ui-preferences.json", body);
        CHECK(!UpdateChannelPreferenceIn(unknown.wstring()));
    }
    // The stored words load as their channels, "prerelease" being Beta.
    for (const auto& word : {std::pair<const char*, UpdateChannel>{"stable", UpdateChannel::Stable}, {"prerelease", UpdateChannel::Beta}, {"nightly", UpdateChannel::Nightly}}) {
        Write(unknown / L"ui-preferences.json", std::string("{\"schemaVersion\":1,\"language\":\"de\",\"updateChannel\":\"") + word.first + "\"}");
        CHECK(UpdateChannelPreferenceIn(unknown.wstring()) == word.second);
    }
    // A field of the wrong type is not a reason to hide the card, or to throw.
    const auto mistyped = root / L"mistyped"; CHECK(std::filesystem::create_directory(mistyped));
    Write(mistyped / L"ui-preferences.json", "{\"schemaVersion\":1,\"language\":\"en\",\"hideGameSettingsCard\":\"yes\"}");
    CHECK(!GameSettingsCardHiddenIn(mistyped.wstring()) && LoadLanguagePreferenceFrom(mistyped.wstring()) == "en");

    // The matches told share the file too: they keep the language and the card, and it keeps them.
    using sf4e::platform::AnnouncedMatch;
    CHECK(LoadAnnouncedMatchesFrom(root.wstring()).empty());
    const std::vector<AnnouncedMatch> told = {{"brg_1", "emt_1", 5000}, {"brg_1", "emt_2", 0}};
    CHECK(SaveAnnouncedMatchesTo(root.wstring(), told, error));
    const auto back = LoadAnnouncedMatchesFrom(root.wstring());
    CHECK(back.size() == 2 && back[0].bridge == "brg_1" && back[0].id == "emt_1" && back[0].until == 5000 && back[1].id == "emt_2" && back[1].until == 0);
    CHECK(LoadLanguagePreferenceFrom(root.wstring()) == "auto" && GameSettingsCardHiddenIn(root.wstring()));
    CHECK(SaveLanguagePreferenceTo(root.wstring(), "fr", error));
    CHECK(LoadAnnouncedMatchesFrom(root.wstring()).size() == 2 && LoadLanguagePreferenceFrom(root.wstring()) == "fr");
    // A service keeps its 64 shown last, and an entry that could not be read back is not written.
    using sf4e::platform::MaxAnnouncedPerService; using sf4e::platform::MaxAnnouncedServices;
    std::vector<AnnouncedMatch> many;
    for (int i = 0; i < 70; ++i) many.push_back({"brg_1", "emt_" + std::to_string(i), 100u + i});
    many.push_back({"brg_1", "", 1});
    many.push_back({"brg_1", std::string(65, 'x'), 1});
    // Another service's entries are bounded on their own: brg_1's do not push them out.
    for (int i = 0; i < 50; ++i) many.push_back({"brg_2", "emt_" + std::to_string(i), 1});
    CHECK(SaveAnnouncedMatchesTo(root.wstring(), many, error));
    const auto kept = LoadAnnouncedMatchesFrom(root.wstring());
    CHECK(kept.size() == MaxAnnouncedPerService + 50 && kept.front().id == "emt_6" && kept[MaxAnnouncedPerService - 1].id == "emt_69" &&
        kept.back().bridge == "brg_2" && kept.back().id == "emt_49");
    CHECK(SaveAnnouncedMatchesTo(root.wstring(), {}, error) && LoadAnnouncedMatchesFrom(root.wstring()).empty());
    // Within a service the oldest go first, by when its list last showed them, not by place; 0 (no clock) is oldest.
    {
        std::vector<AnnouncedMatch> mixed;
        for (int i = 0; i < 66; ++i) mixed.push_back({"brg_1", "emt_" + std::to_string(i), i == 3 ? 0u : i == 5 ? 1u : 1000u});
        sf4e::platform::TrimAnnouncedMatches(mixed);
        CHECK(mixed.size() == MaxAnnouncedPerService && mixed[3].id == "emt_4" && mixed[4].id == "emt_6");
    }
    // Past the services bound, the service whose newest entry is oldest goes whole, never the one kept.
    {
        std::vector<AnnouncedMatch> services;
        for (int s = 0; s < 10; ++s)
            for (int i = 0; i < 2; ++i) services.push_back({"brg_" + std::to_string(s), "emt_" + std::to_string(i), 100u + s * 10 + i});
        auto trimmed = services;
        sf4e::platform::TrimAnnouncedMatches(trimmed);
        CHECK(trimmed.size() == 2 * MaxAnnouncedServices && trimmed.front().bridge == "brg_2" && trimmed.back().bridge == "brg_9");
        trimmed = services;
        sf4e::platform::TrimAnnouncedMatches(trimmed, "brg_0");
        CHECK(trimmed.size() == 2 * MaxAnnouncedServices && trimmed.front().bridge == "brg_0" && trimmed[2].bridge == "brg_3" && trimmed.back().bridge == "brg_9");
    }
    // The bounds fit the file: every service full, with the longest ids, saves and reads back whole.
    {
        std::vector<AnnouncedMatch> largest;
        for (std::size_t s = 0; s < MaxAnnouncedServices; ++s)
            for (std::size_t i = 0; i < MaxAnnouncedPerService; ++i) {
                std::string bridge = std::to_string(s) + "_", id = std::to_string(i) + "_";
                bridge.resize(256, 'b'); id.resize(64, 'm');
                largest.push_back({bridge, id, UINT64_MAX});
            }
        const auto full = root / L"full-matches"; CHECK(std::filesystem::create_directory(full));
        CHECK(SaveAnnouncedMatchesTo(full.wstring(), largest, error));
        const auto back = LoadAnnouncedMatchesFrom(full.wstring());
        CHECK(back.size() == MaxAnnouncedServices * MaxAnnouncedPerService && back.back().id == largest.back().id && back.back().until == UINT64_MAX);
    }
    // A mistyped list, or entry, is dropped without taking the language or other entries with it.
    const auto odd = root / L"odd-matches"; CHECK(std::filesystem::create_directory(odd));
    Write(odd / L"ui-preferences.json", "{\"schemaVersion\":1,\"language\":\"de\",\"announcedMatches\":\"emt_1\"}");
    CHECK(LoadAnnouncedMatchesFrom(odd.wstring()).empty() && LoadLanguagePreferenceFrom(odd.wstring()) == "de");
    Write(odd / L"ui-preferences.json", "{\"schemaVersion\":1,\"language\":\"de\",\"announcedMatches\":[7,{\"id\":\"emt_1\"},"
        "{\"bridge\":\"b\",\"id\":5},{\"bridge\":\"b\",\"id\":\"emt_2\",\"until\":-3},{\"bridge\":\"b\",\"id\":\"emt_3\",\"until\":9}]}");
    const auto some = LoadAnnouncedMatchesFrom(odd.wstring());
    CHECK(some.size() == 2 && some[0].id == "emt_2" && some[0].until == 0 && some[1].id == "emt_3" && some[1].until == 9);
    // Nothing to read from an invalid file, and saving preserves it as it does for the language.
    const auto broken = root / L"broken-matches"; CHECK(std::filesystem::create_directory(broken));
    Write(broken / L"ui-preferences.json", "{");
    CHECK(LoadAnnouncedMatchesFrom(broken.wstring()).empty());
    CHECK(SaveAnnouncedMatchesTo(broken.wstring(), told, error) && Read(broken / L"ui-preferences.json.malformed.bak") == "{" &&
        LoadAnnouncedMatchesFrom(broken.wstring()).size() == 2);

    const std::string invalid[] = {"{", "{\"schemaVersion\":2,\"language\":\"en\"}",
        "{\"schemaVersion\":1,\"language\":\"xx\"}", std::string(256 * 1024 + 1, 'x')};
    for (int i = 0; i < 4; ++i) {
        const auto directory = root / std::to_wstring(i);
        CHECK(std::filesystem::create_directory(directory));
        Write(directory / L"ui-preferences.json", invalid[i]);
        CHECK(LoadLanguagePreferenceFrom(directory.wstring()) == "auto");
        CHECK(Read(directory / L"ui-preferences.json") == invalid[i]);
        CHECK(SaveLanguagePreferenceTo(directory.wstring(), "es-419", error));
        CHECK(Read(directory / L"ui-preferences.json.malformed.bak") == invalid[i]);
        CHECK(LoadLanguagePreferenceFrom(directory.wstring()) == "es-419");
    }

    const auto conflict = root / L"conflict"; CHECK(std::filesystem::create_directory(conflict));
    Write(conflict / L"ui-preferences.json", "{");
    Write(conflict / L"ui-preferences.json.malformed.bak", "different");
    CHECK(!SaveLanguagePreferenceTo(conflict.wstring(), "en", error));
    CHECK(Read(conflict / L"ui-preferences.json") == "{");
    CHECK(Read(conflict / L"ui-preferences.json.malformed.bak") == "different");

    RemoveTempRoot(root);
    std::cout << "UI preference loading, preservation and atomic saving passed\n";
}
