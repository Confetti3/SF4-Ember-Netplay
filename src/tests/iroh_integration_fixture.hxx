#pragma once

// Shared wiring for helper-backed Iroh integration fixtures.  The fixture
// deliberately exercises the same native recovery bridge as the application:
// every peer owns a passive SessionServer, every peer polls RoomRecoveryRuntime
// before stepping its server, and only the committed leader is writable.
#include "../session/IrohRoom.hxx"
#include "../session/RoomRecoveryRuntime.hxx"
#include "../session/sf4e__SessionClient.hxx"
#include "../session/sf4e__SessionServer.hxx"
#include <algorithm>
#include <array>
#include <memory>
#include <iostream>
#include <string>
#include <vector>

namespace sf4e { namespace test {

struct IrohServerPeer {
    std::shared_ptr<session::IrohRoom> room;
    std::unique_ptr<SessionServer> server;
    session::RoomRecoveryRuntime recovery;
};

struct IrohIntegrationPeer : IrohServerPeer {
    std::unique_ptr<SessionClient> client;
    std::string name;
    bool configured = false;
};

inline bool ConfigureIrohIntegrationServer(IrohServerPeer& peer,
    const std::string& sidecar, std::uint8_t capacity = room::MaximumMembers) {
    if (!peer.room || peer.room->GetState() == session::IrohRoom::State::Idle ||
        peer.room->RoomId() == std::array<std::uint8_t, 16>{}) return false;
    const auto roomId = peer.room->RoomId();
    std::string identity = "iroh:";
    constexpr char digits[] = "0123456789abcdef";
    for (const auto byte : roomId) {
        identity += digits[byte >> 4];
        identity += digits[byte & 15];
    }
    peer.server.reset(new SessionServer(identity, sidecar, true, 3, {0, 99}, peer.room->Server()));
    if (!peer.server) return false;
    const auto room = peer.room;
    peer.server->EnableMatchAuthorization(roomId, [room](session::Connection connection) {
        return connection == 1 ? room->LocalIdentity() : room->PeerIdentity(connection);
    }, [room](session::Connection connection) { return room->PeerIncarnation(connection); });
    peer.server->EnableCustomRooms("Iroh recovery fixture", capacity, room->Epoch());
    return true;
}

inline bool ConfigureIrohIntegrationPeer(IrohIntegrationPeer& peer,
    const SessionClient::Callbacks& callbacks, const std::string& sidecar,
    std::uint16_t ggpoPort, std::uint8_t capacity = room::MaximumMembers) {
    if (!ConfigureIrohIntegrationServer(peer, sidecar, capacity)) return false;
    if (peer.name.empty()) peer.name = "Iroh peer";
    peer.client.reset(new SessionClient(callbacks, sidecar, ggpoPort, peer.name));
    peer.client->RequireCustomRooms();
    peer.client->RequireMatchAuthorization();
    if (peer.client->Connect(peer.room->Client(), false) != 0) {
        peer.client.reset();
        peer.server.reset();
        return false;
    }
    peer.configured = true;
    return true;
}

inline bool PumpIrohRecoveryPeers(const std::vector<IrohServerPeer*>& peers) {
    // Poll the room and install the current committed authority before any
    // SessionServer::Step.  A passive server still steps so its transport and
    // recovery gate remain exercised on every helper-backed peer.
    for (auto* peer : peers) {
        if (!peer || !peer->server || !peer->room) return false;
        if(!peer->recovery.Tick(*peer->server,*peer->room)) {
            std::cerr << "Recovery import failed: helper revision=" << peer->room->Coordination().revision
                << " native revision=" << peer->recovery.AppliedRevision() << " error=" << peer->room->Error()
                << " import=" << peer->recovery.Error() << " leader=" << peer->room->Coordination().leaderLocal << '\n';
            return false;
        }
    }
    for (auto* peer : peers) {
        peer->server->AdvanceCustomRoom(GetTickCount64());
        if (peer->server->Step() != 0) {
            std::cerr << "Server step failed: room=" << static_cast<int>(peer->room->GetState())
                << " revision=" << peer->room->Coordination().revision << " error=" << peer->room->Error() << '\n';
            return false;
        }
    }
    return true;
}

inline bool PumpIrohIntegrationPeers(const std::vector<IrohIntegrationPeer*>& peers) {
    std::vector<IrohServerPeer*> servers;
    servers.reserve(peers.size());
    for (auto* peer : peers) {
        if (!peer || !peer->configured || !peer->client) return false;
        servers.push_back(peer);
    }
    if (!PumpIrohRecoveryPeers(servers)) return false;
    for (auto* peer : peers) if (peer->client->Step() != 0) {
        std::cerr << "Client step failed: " << peer->name << " room=" << static_cast<int>(peer->room->GetState())
            << " revision=" << peer->room->Coordination().revision << " error=" << peer->room->Error()
            << " client=" << peer->client->RoomError() << '\n'; return false;
    }
    return true;
}

inline bool PumpIrohIntegrationClients(const std::vector<SessionClient*>& clients) {
    for (std::size_t i=0;i<clients.size();++i) {
        auto* client=clients[i];
        if (!client) return false;
        const auto step=client->Step();
        if (step!=0) {
            const auto& view=client->GetRoomSnapshot();
            std::cerr << "Fixture client " << i << " failed Step=" << step
                << " room_error=" << client->RoomError() << " revision=" << view.revision
                << " local_member=" << view.localMember << " members=" << view.members.size() << '\n';
            return false;
        }
    }
    return true;
}

inline bool AllIrohRoomMembers(const std::vector<IrohIntegrationPeer*>& peers, std::size_t count) {
    return std::all_of(peers.begin(), peers.end(), [count](const IrohIntegrationPeer* peer) {
        return peer && peer->client && peer->client->GetRoomSnapshot().members.size() == count;
    });
}

// These fixtures use synthetic inputs and do not write the user's profile.
// Consuming the committed outcome below is their explicit result-consumption
// boundary; callers must independently prove GGPO and helper retirement.
inline bool AcknowledgeIrohFixtureTerminal(SessionClient& client, std::uint8_t table,
    std::uint64_t generation, bool socketsRetired, bool& observed, bool& queued) {
    // Application consumes action replies on every tick. This also completes
    // SessionClient's owned terminal-ACK retries; continue after local enqueue
    // until the authoritative receipt has committed all recipient ACKs.
    SessionClient::ActionReply reply;
    while (client.TakeActionReply(reply)) {}
    if (queued) return true;
    room::Event event;
    while (client.TakeRoomEvent(event)) {
        if (event.kind == room::Event::Kind::MatchEnded && event.table == table && event.matchGeneration == generation)
            observed = true;
    }
    if (!observed || !socketsRetired) return false;
    queued = client.AcknowledgeTerminal(table, generation) == session::SendResult::Queued;
    return queued;
}

inline bool IrohFixtureTerminalsCommitted(const SessionServer& server,
    const std::vector<std::pair<std::uint8_t, std::uint64_t>>& expected) {
    if (server.HasRecoveryCandidate() || server.PendingProposal()) return false;
    const auto checkpoint = server.RecoveryCheckpoint();
    if (!checkpoint.contains("room") || !checkpoint.at("room").contains("terminal_receipts")) return false;
    const auto& receipts = checkpoint.at("room").at("terminal_receipts");
    for (const auto& key : expected) {
        const auto found = std::find_if(receipts.begin(), receipts.end(), [&](const nlohmann::json& receipt) {
            return receipt.at("table") == key.first && receipt.at("generation") == key.second;
        });
        if (found == receipts.end() || !found->at("acknowledged").get<bool>()) return false;
    }
    return true;
}

} }
