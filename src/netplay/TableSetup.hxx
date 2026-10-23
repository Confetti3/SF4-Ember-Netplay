#pragma once
#include "../common/RoomLimits.hxx"
#include "../common/RoomRules.hxx"

#include <array>
#include <string>

namespace sf4e { namespace netplay {

// What each table of a new room is set to: its rules and its name (empty for
// none). A public room is opened at the public default, a private one at the
// creator's room defaults; a room template can set every table apart.
struct TableSetup {
    std::array<room::Rules, room::TableCount> rules{};
    std::array<std::string, room::TableCount> names{};
    static TableSetup Same(const room::Rules& all) { TableSetup setup; setup.rules.fill(all); return setup; }
    bool Valid() const {
        for (std::size_t i = 0; i < room::TableCount; ++i)
            if (!rules[i].Valid() || names[i].size() > room::MaximumTableNameBytes) return false;
        // As a room name is checked here; the room checks the UTF-8 itself.
        for (const auto& name : names) for (unsigned char c : name) if (c < 32 || c == 127) return false;
        return true;
    }
    bool operator==(const TableSetup& rhs) const { return rules == rhs.rules && names == rhs.names; }
};

} }
