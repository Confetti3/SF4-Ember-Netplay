#include "sf4e__RuntimeSession.hxx"

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

} // namespace ember
