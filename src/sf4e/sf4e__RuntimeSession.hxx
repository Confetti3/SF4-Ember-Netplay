#pragma once
// The room link Ember's runtime owns: its session client, the facts of the
// current match it keeps beside it, and the session server of a room this PC
// hosts. They used to hang off SF4's UserApp as statics, though nothing in
// them is the game's; the runtime creates and drops them, and the game's
// hooks only read them.

#include <cstdint>
#include <memory>
#include <string>

#include "../common/MatchSide.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../session/sf4e__SessionServer.hxx"

namespace ember {

struct RuntimeSession {
    struct Netplay {
        Netplay(
            const sf4e::SessionClient::Callbacks& callbacks,
            std::string sidecarHash,
            uint16_t ggpoPort,
            std::string& name,
            uint8_t _deviceType,
            uint8_t _deviceIdx,
            uint8_t _delay
        );

        sf4e::SessionClient client;
        uint8_t deviceType;
        uint8_t deviceIdx;
        uint8_t delay;
        sf4e::MatchSide matchSides[2];
        // What a spectator's HUD shows for the whole stream: the table's score
        // as the watched game started (see HudScore).
        std::uint32_t startScore[2] = { 0, 0 };
        bool startScoreKnown = false;
        bool spectating = false;
    };

    // Joins the room over `transport`: the room client, with the game's build
    // id and its battle callbacks (game::Battle).
    static void Start(std::unique_ptr<sf4e::session::ClientTransport> transport,
        uint16_t port, std::string& name, uint8_t deviceType, uint8_t deviceIdx, uint8_t delay);

    // Null outside a room.
    static std::unique_ptr<Netplay> netplay;
    // Only while this PC hosts the room.
    static std::unique_ptr<sf4e::SessionServer> server;
};

} // namespace ember
