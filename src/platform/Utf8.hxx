#pragma once
#include <windows.h>
#include <string>

namespace sf4e { namespace platform {
// Strict UTF-8 and UTF-16 conversion. Invalid input yields an empty string.
inline std::wstring Utf8ToWide(const char* value) {
    if (!value) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
    if (length <= 1) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, &result[0], length)) return {};
    result.pop_back();
    return result;
}
inline std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), -1, &result[0], length, nullptr, nullptr)) return {};
    result.pop_back();
    return result;
}
} }
