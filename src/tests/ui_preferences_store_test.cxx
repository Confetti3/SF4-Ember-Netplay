#include "../platform/UiPreferencesStore.hxx"

#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>

#define CHECK(c) do { if (!(c)) { std::cerr << "Check failed at " << __LINE__ << ": " #c << '\n'; std::exit(1); } } while (false)

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
    const auto root = std::filesystem::temp_directory_path() /
        (L"sf4e-ui-preferences-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    CHECK(std::filesystem::create_directory(root));
    std::string error;
    CHECK(LoadLanguagePreferenceFrom(root.wstring()) == "auto");
    CHECK(!std::filesystem::exists(root / L"ui-preferences.json"));
    CHECK(SaveLanguagePreferenceTo(root.wstring(), "pt-BR", error));
    CHECK(LoadLanguagePreferenceFrom(root.wstring()) == "pt-BR");
    CHECK(Read(root / L"ui-preferences.json").find("\"pt-BR\"") != std::string::npos);
    CHECK(SaveLanguagePreferenceTo(root.wstring(), "auto", error));
    CHECK(LoadLanguagePreferenceFrom(root.wstring()) == "auto");
    CHECK(!SaveLanguagePreferenceTo(root.wstring(), "fr", error));
    // The hidden card and the language share the file without overwriting each other.
    CHECK(!GameSettingsCardHiddenIn(root.wstring()));
    CHECK(SaveLanguagePreferenceTo(root.wstring(), "es-419", error));
    CHECK(HideGameSettingsCardIn(root.wstring(), error));
    CHECK(GameSettingsCardHiddenIn(root.wstring()) && LoadLanguagePreferenceFrom(root.wstring()) == "es-419");
    CHECK(SaveLanguagePreferenceTo(root.wstring(), "auto", error));
    CHECK(GameSettingsCardHiddenIn(root.wstring()));
    // A field of the wrong type is not a reason to hide the card, or to throw.
    const auto mistyped = root / L"mistyped"; CHECK(std::filesystem::create_directory(mistyped));
    Write(mistyped / L"ui-preferences.json", "{\"schemaVersion\":1,\"language\":\"en\",\"hideGameSettingsCard\":\"yes\"}");
    CHECK(!GameSettingsCardHiddenIn(mistyped.wstring()) && LoadLanguagePreferenceFrom(mistyped.wstring()) == "en");

    const std::string invalid[] = {"{", "{\"schemaVersion\":2,\"language\":\"en\"}",
        "{\"schemaVersion\":1,\"language\":\"fr\"}", std::string(256 * 1024 + 1, 'x')};
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

    CHECK(std::filesystem::equivalent(root.parent_path(), std::filesystem::temp_directory_path()));
    std::filesystem::remove_all(root);
    std::cout << "UI preference loading, preservation and atomic saving passed\n";
}
