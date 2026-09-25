#pragma once

// How long GGPO lets the opponent's fighter link stay silent before it ends
// the match. Both fighters of a room run the same build, so both apply it.
//
// A drop shorter than TimeoutMs pauses the fight at GGPO's prediction limit
// behind the "Connection unstable" countdown, which starts NotifyMs into the
// silence, and then resumes. An rc1 set lost a match to a 5 s drop in both
// directions that room control rode out in the same window; 8 s covers that.
//
// The outer bounds it must stay under, all longer than 8 s:
//  - iroh abandons a direct path after 15 s idle (a relay path after 30 s),
//    and the QUIC connection closes only when its last path is abandoned
//    (30 s connection idle). The game link then fails through the helper's
//    bridge into IrohMatchSession's gameplay_connection_lost.
//  - openraft's 10-12 s election timeout; a term change leaves a started
//    match alone.
//
// P1's spectator streams inherit the value, but SpectatorPolicy can drop a
// spectator sooner. A spectator client's own link has no silence timeout:
// GGPO's spectator backend rejects the setting.

#include <ggponet.h>

namespace sf4e { namespace GgpoDisconnectTolerance {

constexpr int TimeoutMs = 8000;
constexpr int NotifyMs = 1500;
static_assert(NotifyMs < TimeoutMs, "the countdown must start before the match ends");

inline void Apply(GGPOSession* session) {
	ggpo_set_disconnect_timeout(session, TimeoutMs);
	ggpo_set_disconnect_notify_start(session, NotifyMs);
}

} }
