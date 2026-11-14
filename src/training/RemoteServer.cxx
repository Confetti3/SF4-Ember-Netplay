#include "RemoteServer.hxx"
#include "CameraControl.hxx"
#include "RemotePage.hxx"
#include "RemoteHud.hxx"
#include "RemoteHudPage.hxx"
#include "../sf4e/sf4e__GameThemes.hxx"
#include "../sf4e/sf4e__NetplayFacade.hxx"
#include "ComboStore.hxx"
#include "GameFiles.hxx"
#include "RemoteProtocol.hxx"
#include "RemoteRequest.hxx"
#include "FrameDataTable.hxx"
#include "TrainingRuntime.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include "../ui/TrainingPanel.hxx"
#include "../common/JsonDump.hxx"
#include "../common/JsonField.hxx"
#include <winsock2.h>
#include <shellapi.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

namespace sf4e { namespace training { namespace remote {
namespace {
constexpr unsigned short FirstPort = 47321;
constexpr int PortTries = 8, MaxStreams = 8, MaxRequests = 32;
// How long Stop waits for the requests and streams it cut off to end. It runs on the render thread and as the
// game closes, so a handler still busy after this is not waited for.
constexpr std::chrono::seconds StopWait{1};
const char* const HostName = "ember.local";
std::atomic<bool> s_running{false};
std::atomic<int> s_streams{0}, s_requests{0};
std::atomic<int> s_loops{0};  // the two loops run detached, so no thread object can outlive the process and end it in std::terminate
SOCKET s_listen = INVALID_SOCKET, s_dns = INVALID_SOCKET;
unsigned short s_port = 0;
PinGate s_pins;
BodyBudget s_bodies{BodiesHeld};
// The sockets of the requests and streams under way, so Stop can end every wait on them at once.
std::mutex s_clientLock;
std::vector<SOCKET> s_clients;
void Hold(SOCKET socket) { std::lock_guard<std::mutex> hold(s_clientLock); s_clients.push_back(socket); }
void Close(SOCKET socket) {
    { std::lock_guard<std::mutex> hold(s_clientLock); s_clients.erase(std::remove(s_clients.begin(), s_clients.end(), socket), s_clients.end()); }
    closesocket(socket);
}
void EndClients() { std::lock_guard<std::mutex> hold(s_clientLock); for (const SOCKET socket : s_clients) shutdown(socket, SD_BOTH); }

void Send(SOCKET socket, const Reply& reply) {
    const int status = reply.status;
    const char* reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 403 ? "Forbidden" : status == 409 ? "Conflict" : status == 429 ? "Too Many Requests" :
        status == 500 ? "Internal Server Error" : status == 503 ? "Service Unavailable" : "Not Found";
    const std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " + reply.type +
        "\r\nContent-Length: " + std::to_string(reply.body.size()) + "\r\nCache-Control: no-store\r\n" + reply.headers + "Connection: close\r\n\r\n";
    send(socket, head.data(), static_cast<int>(head.size()), 0);
    send(socket, reply.body.data(), static_cast<int>(reply.body.size()), 0);
}
// Waits for the client's next bytes until `deadline`, in short waits so a stopping server is seen: how many
// came, or 0 when the client closed, the time ran out or the server is stopping.
int Receive(SOCKET socket, char* buffer, int size, Clock::time_point deadline) {
    for (;;) {
        const auto wait = NextWait(Clock::now(), deadline);
        if (!s_running || wait.count() <= 0) return 0;
        fd_set set; FD_ZERO(&set); FD_SET(socket, &set);
        timeval time{0, static_cast<long>(wait.count()) * 1000};
        const int ready = select(0, &set, nullptr, nullptr, &time);
        if (ready < 0) return 0;
        if (ready > 0) { const int got = recv(socket, buffer, size, 0); return got > 0 ? got : 0; }
    }
}
std::string Words(const char* key, int arg) { return arg < 0 ? std::string(loc::T(key)) : loc::Tf(key, arg); }
// One phone's live stream: what changed since the last message, about 20 times a second. It ends when the
// phone goes or the server stops. ponytail: each stream reads the view itself; one shared reader if phones multiply.
void Feed(SOCKET socket) {
    DWORD timeout = 500; setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: keep-alive\r\n\r\nretry: 2000\n\n";
    bool alive = send(socket, head.data(), static_cast<int>(head.size()), 0) > 0;
    MeterFeed feed; nlohmann::json known = nlohmann::json::object();
    std::string lines, data, live;
    auto quiet = std::chrono::steady_clock::now();
    while (alive && s_running) {
        const auto view = ReadView();
        const auto values = ValuesJson(view, Phone().Values());
        nlohmann::json message = nlohmann::json::object();
        if (const auto changed = Changed(known, values); !changed.empty()) message["values"] = changed;
        if (auto meter = feed.Next(view.meter); !meter.is_null()) message["meter"] = std::move(meter);
        const auto sides = std::string(ui::TrainingFrameData(view.meter, 0)) + '\n' + ui::TrainingFrameData(view.meter, 1);
        if (sides != lines) { lines = sides; message["lines"] = {ui::TrainingFrameData(view.meter, 0), ui::TrainingFrameData(view.meter, 1)}; }
        const auto readings = DataJson(view), extras = LiveJson(view);
        if (const auto now = DumpJson(readings); now != data) { data = now; message["data"] = readings; }
        if (const auto now = DumpJson(extras); now != live) { live = now; message["live"] = extras; }
        const auto now = std::chrono::steady_clock::now();
        std::string frame = message.empty() ? std::string() : EventFrame(message);
        if (frame.empty() && now - quiet > std::chrono::seconds(10)) frame = ": ping\n\n";
        if (!frame.empty()) { quiet = now; alive = send(socket, frame.data(), static_cast<int>(frame.size()), 0) > 0; }
        // A phone that left (page closed, screen off) is seen at once, not at the next message, so its place is free for the reconnect.
        fd_set gone; FD_ZERO(&gone); FD_SET(socket, &gone); timeval none{0, 0};
        char byte;
        if (alive && select(0, &gone, nullptr, nullptr, &none) > 0 && recv(socket, &byte, 1, 0) <= 0) alive = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
// A stream's thread: nothing it meets may end the game, so whatever it throws ends the stream with a line in the log.
void Stream(SOCKET socket) {
    try { Feed(socket); }
    catch (const std::exception& e) { spdlog::warn("Phone remote: a stream ended on an error: {}", e.what()); }
    Close(socket);
    --s_streams;
}
// Hands the socket to a stream of its own; false, answered, when the streams are all taken.
bool OpenStream(SOCKET socket) {
    if (++s_streams > MaxStreams) { --s_streams; Send(socket, TextReply(429, "streams")); return false; }
    try { std::thread(Stream, socket).detach(); }
    catch (...) { --s_streams; throw; }
    return true;
}
// Ember's frame data (assets/framedata/usf4.json), embedded in this binary (FrameDataTable.hxx), never a package
// file: an older updater refuses a package file it does not know. Parsed once; null when it is bad.
const nlohmann::json& FrameData() {
    static const nlohmann::json data = nlohmann::json::parse(frameDataTable, frameDataTable + sizeof(frameDataTable) - 1, nullptr, false);
    return data;
}
std::string FighterName(int id) { const auto* fighter = selection::FindFighter(id); return fighter ? fighter->name : std::string(); }
// A combo order from the phone: save the last recorded one, rename, delete or play a saved one.
nlohmann::json ComboOrder(const nlohmann::json& body, const std::string& cmd) {
    const auto fail = [](const char* why) { return nlohmann::json{{"ok", false}, {"error", why}}; };
    auto& store = Combos();
    const auto id = JsonField<std::string>(body, "id");
    std::string name, error;
    if (cmd == "comboSave") {
        const auto last = LastCombo();
        if (last.frames.empty()) return fail("no combo recorded");
        if (!ParseComboName(JsonField<std::string>(body, "name"), name, error)) return fail("bad name");
        SavedCombo combo; combo.name = name; combo.fighter = last.fighter; combo.hits = (std::min)(last.hits, 999);
        combo.x[0] = last.x[0]; combo.x[1] = last.x[1]; combo.frames = last.frames;
        std::string saved;
        return store.Add(std::move(combo), saved, error) ? nlohmann::json{{"ok", true}, {"id", saved}} : fail("could not save");
    }
    if (!id) return fail("no id");
    if (cmd == "comboDelete") return {{"ok", store.Delete(*id)}};
    if (cmd == "comboRename") {
        if (!ParseComboName(JsonField<std::string>(body, "name"), name, error)) return fail("bad name");
        return store.Rename(*id, name, error) ? nlohmann::json{{"ok", true}} : fail("could not rename");
    }
    SavedCombo combo;
    const auto view = ReadView();
    if (!view.available || !store.Load(*id, combo)) return fail("cannot play");
    bool ok = true;
    for (auto& command : ComboPlayCommands(combo, view.generation)) ok &= Submit(std::move(command));
    return {{"ok", ok}};
}
// {"cmd": ...} or {"set": ...}: to the game, the overlay or the combo store, as ParseOrder routes it.
Reply PhoneOrder(const std::string& text) {
    const auto body = nlohmann::json::parse(text, nullptr, false);
    Order order; std::string error;
    switch (ParseOrder(body, ReadView(), order, error)) {
    case Route::Refused: return JsonReply({{"ok", false}, {"error", error}}, 400);
    case Route::Store: return JsonReply(ComboOrder(body, order.id));
    case Route::Game: return JsonReply({{"ok", Submit(std::move(order.command))}});
    case Route::Overlay: return JsonReply({{"ok", order.id == "replyMoves" ? Phone().PushText(order.id, order.text) : Phone().Push(order.id, order.value)}});
    }
    return TextReply(404, "no");
}
Reply CameraOrder(const std::string& text) {
    std::string problem;
    const auto answer = camera::Phone().Order(nlohmann::json::parse(text, nullptr, false), problem);
    return answer.is_null() ? JsonReply({{"ok", false}, {"error", problem}}, 400) : JsonReply(answer);
}
Reply ComboFile(const std::string& id) {
    Reply reply;
    if (!Combos().Bytes(id, reply.body)) return TextReply(404, "no");
    reply.status = 200;
    reply.headers = "Content-Disposition: attachment; filename=\"ember-combo-" + id + ".json\"\r\n";
    return reply;
}
// ?fighter=Name gives that fighter's rows; none, the fighters.
Reply Frames(const std::string& query) {
    const std::string fighter = QueryParam(query, "fighter");
    const auto frames = fighter.empty() ? FramesIndex(FrameData()) : FramesOf(FrameData(), fighter);
    return frames.is_null() ? TextReply(404, "no") : JsonReply(frames);
}
const hud::Host& HudHost() {
    static const hud::Host host{ [] { return sf4e::GameThemes::Directory(); },
        [](const std::string& name, std::map<std::string, std::string> files) { sf4e::GameThemes::ApplyHud(name, std::move(files)); }, [] { sf4e::GameThemes::Restart(); } };
    return host;
}
// The answer to a request its route takes (a stream is handed its socket instead).
Reply Answer(const Resource& resource, const Head& head, const std::string& body) {
    if (const auto* order = std::get_if<hud::Order>(&resource.handler)) return hud::Handle(HudHost(), *order, head.query, body, head.userAgent);
    switch (std::get<Endpoint>(resource.handler)) {
    case Endpoint::Page: { static const std::string page = PageWithHudEditor(); Reply reply; reply.status = 200; reply.type = "text/html; charset=utf-8"; reply.body = page; return reply; }
    case Endpoint::Options: return JsonReply(OptionsJson(Words));
    case Endpoint::State: return JsonReply(ValuesJson(ReadView(), Phone().Values()));
    case Endpoint::Command: return PhoneOrder(body);
    case Endpoint::Camera: return JsonReply(camera::Phone().State());
    case Endpoint::CameraOrder: return CameraOrder(body);
    case Endpoint::Combos: return JsonReply(CombosJson(Combos().List(), FighterName));
    case Endpoint::ComboImport: { std::string id, error; const bool ok = Combos().Import(body, id, error); return JsonReply({{"ok", ok}, {"id", id}, {"error", error}}, ok ? 200 : 400); }
    case Endpoint::ComboFile: return ComboFile(head.path.substr(std::strlen(resource.path)));
    case Endpoint::Frames: return Frames(head.query);
    case Endpoint::Stream: break;
    }
    return TextReply(404, "no");
}
// Whether what a route needs holds now.
bool Holds(Needs needs) {
    switch (needs) {
    case Needs::Nothing: return true;
    case Needs::NoMatch: {
        const auto runtime = NetplayFacade::GetRuntimeSnapshotShared();
        return runtime->session.match == netplay::MatchState::None && !runtime->foughtGame.Active() && !runtime->watchedGame.Active();
    }
    case Needs::Camera: return camera::Phone().TakesOrders();
    }
    return false;
}
// Answers one request; a stream takes the socket over and returns true. The head comes first, within HeadBytes
// and HeadTime; the route, the PIN and the body's length are settled from it before any of the body is read;
// then the body, within the route's limit, the bodies' budget and RequestTime.
bool Respond(SOCKET socket, unsigned long peer) {
    DWORD timeout = 5000; setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    const auto start = Clock::now();
    std::string request; char buffer[2048];
    std::size_t end;
    while ((end = request.find("\r\n\r\n")) == std::string::npos) {
        if (request.size() > HeadBytes) { Send(socket, TextReply(400, "too big")); return false; }
        const int got = Receive(socket, buffer, sizeof(buffer), start + HeadTime);
        if (got <= 0) return false;
        request.append(buffer, got);
    }
    Head head;
    if (end > HeadBytes || !ParseHead(request.substr(0, end), head)) { Send(socket, TextReply(400, "bad request")); return false; }
    const Admission admission = Admit(head, s_pins, peer, Clock::now(), Holds);
    if (admission.refused) { Send(socket, *admission.refused); return false; }
    const Resource& resource = *admission.resource;
    if (const auto* endpoint = std::get_if<Endpoint>(&resource.handler); endpoint && *endpoint == Endpoint::Stream) return OpenStream(socket);
    const BodyBudget::Lease lease(s_bodies, head.length);
    if (!lease) { Send(socket, JsonReply({{"ok", false}, {"error", "busy, try again"}}, 503)); return false; }
    std::string body = request.substr(end + 4, head.length);
    request = std::string();
    body.reserve(head.length);
    while (body.size() < head.length) {
        const int got = Receive(socket, buffer, static_cast<int>((std::min)(sizeof(buffer), head.length - body.size())), start + RequestTime);
        if (got <= 0) return false;
        body.append(buffer, got);
    }
    Send(socket, Answer(resource, head, body));
    return false;
}
// A request's thread: nothing a request does may end the game, so whatever it throws is answered 500 with a line
// in the log. The answer is fixed text, so sending it cannot fail in turn.
bool Serve(SOCKET socket, unsigned long peer) {
    try { return Respond(socket, peer); }
    catch (const std::exception& e) {
        spdlog::warn("Phone remote: a request failed: {}", e.what());
        static const char failed[] = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(socket, failed, static_cast<int>(sizeof(failed) - 1), 0);
        return false;
    }
}
void HttpLoop() {
    while (s_running) {
        fd_set set; FD_ZERO(&set); FD_SET(s_listen, &set);
        timeval wait{0, 200000};
        if (select(0, &set, nullptr, nullptr, &wait) <= 0) continue;
        sockaddr_in peer{}; int size = sizeof(peer);
        const SOCKET socket = accept(s_listen, reinterpret_cast<sockaddr*>(&peer), &size);
        if (socket == INVALID_SOCKET) continue;
        const auto from = ntohl(peer.sin_addr.s_addr);
        if (!PrivatePeer(from)) { closesocket(socket); continue; }
        if (++s_requests > MaxRequests) { --s_requests; closesocket(socket); continue; }
        // A thread each: a phone's browser opens idle spare connections, and one waiting for its request must not hold up the rest.
        Hold(socket);
        try { std::thread([socket, from] { if (!Serve(socket, from)) Close(socket); --s_requests; }).detach(); }
        catch (const std::exception&) { Close(socket); --s_requests; }
    }
}
// Answers an A query for ember.local with the address this PC has on the route to the asker.
void MdnsLoop() {
    while (s_running) {
        fd_set set; FD_ZERO(&set); FD_SET(s_dns, &set);
        timeval wait{0, 200000};
        if (select(0, &set, nullptr, nullptr, &wait) <= 0) continue;
        unsigned char packet[512]; sockaddr_in from{}; int size = sizeof(from);
        const int got = recvfrom(s_dns, reinterpret_cast<char*>(packet), sizeof(packet), 0, reinterpret_cast<sockaddr*>(&from), &size);
        // Header 12 bytes, then "\5ember\5local\0", type A or ANY, class IN; a response (QR set) is not a question.
        static const unsigned char name[] = {5, 'e', 'm', 'b', 'e', 'r', 5, 'l', 'o', 'c', 'a', 'l', 0};
        if (got < 12 + static_cast<int>(sizeof(name)) + 4 || (packet[2] & 0x80) || !PrivatePeer(ntohl(from.sin_addr.s_addr))) continue;
        if (std::memcmp(packet + 12, name, sizeof(name)) != 0) continue;
        const int type = packet[12 + sizeof(name)] << 8 | packet[13 + sizeof(name)];
        if (type != 1 && type != 255) continue;
        SOCKET probe = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in route = from; route.sin_port = htons(9);
        sockaddr_in mine{}; int mineSize = sizeof(mine);
        const bool found = probe != INVALID_SOCKET && connect(probe, reinterpret_cast<sockaddr*>(&route), sizeof(route)) == 0 && getsockname(probe, reinterpret_cast<sockaddr*>(&mine), &mineSize) == 0;
        if (probe != INVALID_SOCKET) closesocket(probe);
        if (!found) continue;
        unsigned char answer[64] = {packet[0], packet[1], 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0};
        std::size_t at = 12;
        std::memcpy(answer + at, name, sizeof(name)); at += sizeof(name);
        const unsigned char tail[] = {0, 1, 0x80, 1, 0, 0, 0, 120, 0, 4};
        std::memcpy(answer + at, tail, sizeof(tail)); at += sizeof(tail);
        std::memcpy(answer + at, &mine.sin_addr, 4); at += 4;
        // Unicast to the asker: enough for a one-shot lookup, and spares the multicast group.
        sendto(s_dns, reinterpret_cast<const char*>(answer), static_cast<int>(at), 0, reinterpret_cast<sockaddr*>(&from), sizeof(from));
    }
}
SOCKET OpenMdns() {
    SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket == INVALID_SOCKET) return socket;
    BOOL reuse = TRUE; setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in any{}; any.sin_family = AF_INET; any.sin_port = htons(5353);
    ip_mreq group{}; inet_pton(AF_INET, "224.0.0.251", &group.imr_multiaddr);
    if (bind(socket, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0 ||
        setsockopt(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&group), sizeof(group)) != 0) { closesocket(socket); return INVALID_SOCKET; }
    return socket;
}
} // namespace

bool PrivatePeer(unsigned long a) {
    return (a >> 24) == 127 || (a >> 24) == 10 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8 || (a >> 16) == 0xA9FE;
}
bool Start() {
    if (s_running) return true;
    if (s_loops > 0) return false;  // the last run's loops are still ending
    WSADATA data; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    unsigned char random[4]; unsigned number = 0;
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
    std::memcpy(&number, random, 4);
    char pin[8]; std::snprintf(pin, sizeof(pin), "%06u", number % 1000000);
    s_pins.Reset(pin);
    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen == INVALID_SOCKET) return false;
    for (int i = 0; i < PortTries; ++i) {
        sockaddr_in at{}; at.sin_family = AF_INET; at.sin_addr.s_addr = htonl(INADDR_ANY); at.sin_port = htons(static_cast<unsigned short>(FirstPort + i));
        if (bind(s_listen, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0 && listen(s_listen, 16) == 0) { s_port = FirstPort + i; break; }
    }
    if (!s_port) { closesocket(s_listen); s_listen = INVALID_SOCKET; return false; }
    s_dns = OpenMdns();  // without it the phone types the PC's address; the page and PIN work the same
    s_running = true;
    ++s_loops; std::thread([] { HttpLoop(); --s_loops; }).detach();
    if (s_dns != INVALID_SOCKET) { ++s_loops; std::thread([] { MdnsLoop(); --s_loops; }).detach(); }
    return true;
}
void Stop() {
    if (!s_running.exchange(false)) return;
    hud::Allow(false);
    s_pins.Reset({});
    // Every wait on a phone ends at once, and the loops see s_running within their 200 ms wait. Bounded: a
    // handler still busy is left to end on its own.
    EndClients();
    const auto until = Clock::now() + StopWait;
    while ((s_loops > 0 || s_streams > 0 || s_requests > 0) && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    closesocket(s_listen); if (s_dns != INVALID_SOCKET) closesocket(s_dns);
    s_listen = s_dns = INVALID_SOCKET; s_port = 0;
}
// The PC's private IPv4 address on a network with a gateway (the LAN the phone is on), or empty.
std::string LanAddress() {
    ULONG size = 16384; std::vector<char> buffer(size);
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size) != NO_ERROR) return {};
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || !adapter->FirstGatewayAddress) continue;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            const auto* at = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
            const auto host = ntohl(at->sin_addr.s_addr);
            if (!PrivatePeer(host) || (host >> 24) == 127) continue;
            char text[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &at->sin_addr, text, sizeof(text));
            return text;
        }
    }
    return {};
}
std::string Pin() { return s_running ? s_pins.Pin() : std::string(); }
std::string Link() { const auto url = Url(); return url.empty() ? url : url + "/?pin=" + Pin(); }
bool Open() {
    const auto link = Link();
    return !link.empty() && reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", link.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
// The IPv4 address, since the name needs mDNS and many networks and phones do not resolve it.
std::string Url() {
    if (!s_running) return {};
    const auto lan = LanAddress();
    return std::string("http://") + (lan.empty() ? HostName : lan) + ":" + std::to_string(s_port);
}
} } }
