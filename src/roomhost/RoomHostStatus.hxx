#pragma once

// The room host's status line for the supervisor (rust/ember-rooms/src/
// protocol.rs). Plain inputs and no session code, so a test can build the
// largest line without a helper.
#include <cstddef>
#include <string>
#include <vector>

namespace sf4e { namespace roomhost {

// The supervisor kills a child whose line passes 64 KiB; the host stays well
// under it. A room holds at most room::MaximumKickedAccounts (512) bans, each
// an Ember ID of 57 bytes, so a full line is about 31 KB plus the invitation.
constexpr std::size_t MaximumStatusLineBytes = 60000;

std::string StatusLine(std::size_t members, std::size_t tablesPlaying, const std::string& invitation,
	const std::vector<std::string>& banned);

} }
