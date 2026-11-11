#pragma once
// Times in this PC's time zone, as strftime writes them: one place for the
// replay names, the sent reports and the tournament desk.
#include <cstdint>
#include <ctime>
#include <string>

namespace sf4e {
inline bool LocalTm(std::time_t at, std::tm& local) {
#ifdef _WIN32
    return localtime_s(&local, &at) == 0;
#else
    return localtime_r(&at, &local) != nullptr;
#endif
}
// Empty when the time cannot be read.
inline std::string LocalTime(std::uint64_t at, const char* format = "%Y-%m-%d %H:%M") {
    std::tm local = {};
    char text[32] = {};
    return LocalTm(static_cast<std::time_t>(at), local) && std::strftime(text, sizeof text, format, &local) ? text : std::string();
}
}
