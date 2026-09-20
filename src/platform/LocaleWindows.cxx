#include "LocaleWindows.hxx"

#define NOMINMAX
#include <windows.h>
#include <cwchar>

namespace sf4e { namespace platform {
std::vector<std::string> WindowsUiLanguages() {
    ULONG count = 0, characters = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &characters) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || !characters) return {};
    std::vector<wchar_t> buffer(characters);
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &characters)) return {};
    std::vector<std::string> result;
    for (const wchar_t* value = buffer.data(); *value; value += std::wcslen(value) + 1) {
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 1) continue;
        std::string utf8(static_cast<std::size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, &utf8[0], bytes, nullptr, nullptr)) {
            utf8.pop_back(); result.push_back(std::move(utf8));
        }
    }
    return result;
}
} }
