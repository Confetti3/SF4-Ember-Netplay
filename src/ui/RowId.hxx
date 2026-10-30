#pragma once
#include <charconv>
#include <string_view>
#include <system_error>
namespace sf4e { namespace ui {
// The number a keyed row id carries after its prefix ("cabinet-7" with
// "cabinet-" is 7), or -1 when the id has another prefix, no number or more
// than a number ("table-name" with "table-"). A row whose id does not parse
// then does nothing, where std::stoi would end the process (EMBER-68).
inline int RowIndex(std::string_view id, std::string_view prefix) {
    if (id.size() <= prefix.size() || id.compare(0, prefix.size(), prefix) != 0) return -1;
    int value = 0;
    const char* begin = id.data() + prefix.size();
    const char* end = id.data() + id.size();
    const auto parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc() && parsed.ptr == end && value >= 0 ? value : -1;
}
} }
