#pragma once
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>
namespace sf4e { namespace ui {
// The number a keyed row id carries after its prefix, as a `Key`
// ("cabinet-7" with "cabinet-" is 7; "member-<id>" a 64-bit room::MemberId),
// or nothing when the id has another prefix, no number, a sign, more than a
// number ("table-name" with "table-") or a number `Key` cannot hold. A row
// whose id does not parse then does nothing, where std::stoi would end the
// process (EMBER-68). Read it once, with its bound:
// `if (const auto t = RowKey<std::size_t>(id, "table-"); t && *t < tables.size())`.
template <class Key = std::uint64_t>
std::optional<Key> RowKey(std::string_view id, std::string_view prefix) {
    static_assert(std::is_integral_v<Key> && !std::is_same_v<Key, bool>, "a row key is a number");
    if (id.size() <= prefix.size() || id.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    std::uint64_t value = 0;
    const char* begin = id.data() + prefix.size();
    const char* end = id.data() + id.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc() || parsed.ptr != end ||
        value > static_cast<std::uint64_t>((std::numeric_limits<Key>::max)())) return std::nullopt;
    return static_cast<Key>(value);
}
} }
