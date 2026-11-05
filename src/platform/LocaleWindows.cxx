#include "LocaleWindows.hxx"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "Utf8.hxx"
#include <cwchar>
#include <mutex>

namespace sf4e { namespace platform {
namespace {
std::mutex gameLanguageMutex;
std::string gameLanguage;
}

std::vector<std::string> WindowsUiLanguages() {
    ULONG count = 0, characters = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &characters) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || !characters) return {};
    std::vector<wchar_t> buffer(characters);
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &characters)) return {};
    std::vector<std::string> result;
    for (const wchar_t* value = buffer.data(); *value; value += std::wcslen(value) + 1)
        if (auto utf8 = WideToUtf8(value); !utf8.empty()) result.push_back(std::move(utf8));
    return result;
}

std::string SteamClientLanguage() {
    wchar_t value[64] = {};
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"Language", RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS)
        return {};
    return WideToUtf8(value);
}

void SetGameLanguage(std::string code) {
    std::lock_guard<std::mutex> lock(gameLanguageMutex);
    gameLanguage = std::move(code);
}

std::string GameLanguage() {
    std::lock_guard<std::mutex> lock(gameLanguageMutex);
    return gameLanguage;
}

loc::Locale ResolveUiLocale(std::string_view preference) {
    return loc::ResolveLocale(preference, GameLanguage(), WindowsUiLanguages());
}

CountryCode WindowsCountry() {
    const GEOID nation = GetUserGeoID(GEOCLASS_NATION);
    if (nation == GEOID_NOT_AVAILABLE) return {};
    wchar_t code[8] = {};
    if (!GetGeoInfoW(nation, GEO_ISO2, code, static_cast<int>(sizeof(code) / sizeof(code[0])), 0)) return {};
    return CountryFromGeoIso2(code);
}
} }
