#include "sf4e__RuntimeSession.hxx"

#include "../common/Localization.hxx"
#include "sf4e__GameAdapter.hxx"
#include "sf4e__NetplayFacade.hxx"

namespace ember {

std::unique_ptr<RuntimeSession::Netplay> RuntimeSession::netplay;
std::unique_ptr<sf4e::SessionServer> RuntimeSession::server;

RuntimeSession::Netplay::Netplay(
    const sf4e::SessionClient::Callbacks& callbacks,
    std::string sidecarHash,
    uint16_t ggpoPort,
    std::string& name,
    uint8_t _deviceType,
    uint8_t _deviceIdx,
    uint8_t _delay
):
    client(callbacks, sidecarHash, ggpoPort, name),
    deviceType(_deviceType),
    deviceIdx(_deviceIdx),
    delay(_delay)
{}

namespace {
using sf4e::SessionClient;
// A refused join, worded for the way the room was entered.
void OnClientError(SessionClient::ErrorType type, SessionClient* const, const SessionClient::Callbacks&) {
    if (const char* message = sf4e::loc::T(sf4e::NetplayFacade::IsRuntimePublicJoin() ?
        SessionClient::PublicJoinRejectionKey(type) : SessionClient::JoinRejectionKey(type)))
        sf4e::NetplayFacade::SetLastError(message);
}
void OnReady(SessionClient* const, const SessionClient::Callbacks&) { game::Game().battle.OnSessionReady(); }
void OnBattleSynced(SessionClient* const, const SessionClient::Callbacks&) { game::Game().battle.OnSessionSynced(); }
}

void RuntimeSession::Start(std::unique_ptr<sf4e::session::ClientTransport> transport,
    uint16_t port, std::string& name, uint8_t deviceType, uint8_t deviceIdx, uint8_t delay) {
    // IrohMatchSession requires authorization and suppresses legacy all-ready.
    // The normal battle-loaded synchronization callback remains shared.
    const SessionClient::Callbacks callbacks = { nullptr, OnClientError, OnReady, OnBattleSynced };
    netplay.reset(new Netplay(callbacks, game::Game().state.BuildId(), port, name, deviceType, deviceIdx, delay));
    netplay->client.Connect(std::move(transport));
}

} // namespace ember
