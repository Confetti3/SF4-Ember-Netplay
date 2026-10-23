#pragma once
#include "../common/UiAccent.hxx"
#include <nlohmann/json.hpp>

namespace sf4e { namespace netplay {
// A bad color does not prevent the remaining settings from loading.
inline unsigned ReadUiAccent(const nlohmann::json& saved) {
    if (!saved.is_object()) return accent::Default;
    const auto found = saved.find("uiAccent");
    if (found == saved.end() || !found->is_number_integer()) return accent::Default;
    if (found->is_number_unsigned()) {
        const auto value = found->get<std::uint64_t>();
        return value <= accent::Maximum ? static_cast<unsigned>(value) : accent::Default;
    }
    const auto value = found->get<std::int64_t>();
    return value >= 0 && value <= accent::Maximum ? static_cast<unsigned>(value) : accent::Default;
}
} }
