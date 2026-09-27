#include "LocaleWindows.hxx"
#include "../common/GameLanguage.hxx"

#define NOMINMAX
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

void SetGameDirectory(const std::filesystem::path& directory) {
    auto language = directory.empty() ? std::string() : loc::DetectGameLanguage(directory, SteamClientLanguage());
    std::lock_guard<std::mutex> lock(gameLanguageMutex);
    gameLanguage = std::move(language);
}

std::string GameLanguage() {
    std::lock_guard<std::mutex> lock(gameLanguageMutex);
    return gameLanguage;
}

loc::Locale ResolveUiLocale(std::string_view preference) {
    return loc::ResolveLocale(preference, GameLanguage(), WindowsUiLanguages());
}
} }
