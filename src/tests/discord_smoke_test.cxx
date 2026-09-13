#include "../platform/HelperClient.hxx"
#include <nlohmann/json.hpp>
#include <iostream>

// Publishes only Starting Ember, then disables activity and checks orderly exit.
// No invitations are created, consumed, printed, or sent to another person.
int wmain(int argc, wchar_t** argv) {
    using namespace sf4e::platform;
    if (argc != 2) return 1;
    HelperProcess companion;
    if (!companion.Start(argv[1], GetCurrentProcessId())) return 2;
    HelperClient client;
    if (!client.Start(companion.Bootstrap())) return 3;
    const auto deadline = GetTickCount64() + 45000;
    bool available = false, registered = false;
    ULONGLONG published = 0;
    nlohmann::json presence{{"type","presence"},{"epoch",0},{"show",true},
        {"activity","Starting Ember"},{"party",""},{"size",0},{"capacity",0},{"secret",""},{"expires",0}};
    while (GetTickCount64() < deadline && client.State() != HelperState::Failed) {
        if (client.State() == HelperState::Connected && GetTickCount64() - published > 1000) {
            client.Send(presence.dump()); published = GetTickCount64();
        }
        HelperMessage message;
        while (client.TryReceive(message)) {
            const auto event = nlohmann::json::parse(message.payload);
            if (event.value("type","") == "status") {
                available = event.value("available",false);
                registered = event.value("registered",false);
            }
        }
        if (available) break;
        Sleep(10);
    }
    presence["show"] = false;
    client.Send(presence.dump());
    Sleep(500);
    client.Send("{\"type\":\"shutdown\"}");
    const auto exitDeadline = GetTickCount64() + 5000;
    while (companion.IsRunning() && GetTickCount64() < exitDeadline) Sleep(10);
    const bool clean = !companion.IsRunning();
    client.Stop();
    std::cout << "Discord RPC=" << available << " launch registration=" << registered << " clean exit=" << clean << '\n';
    return available && registered && clean ? 0 : 4;
}
