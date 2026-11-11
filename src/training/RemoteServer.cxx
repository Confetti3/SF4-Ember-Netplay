#include "RemoteServer.hxx"
#include "CameraControl.hxx"
#include "RemotePage.hxx"
#include "RemoteHud.hxx"
#include "RemoteHudPage.hxx"
#include "../sf4e/sf4e__GameThemes.hxx"
#include "ComboStore.hxx"
#include "GameFiles.hxx"
#include "RemoteProtocol.hxx"
#include "FrameDataTable.hxx"
#include "TrainingRuntime.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/Localization.hxx"
#include "../ui/TrainingPanel.hxx"
#include <winsock2.h>
#include <shellapi.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace sf4e { namespace training { namespace remote {
namespace {
constexpr unsigned short FirstPort = 47321;
constexpr std::size_t HudBytes = 65536, HudImageBytes = 6u << 20;
constexpr int PortTries = 8, FailuresAllowed = 5, LockSeconds = 30, MaxStreams = 8, MaxRequests = 32;
const char* const HostName = "ember.local";
std::atomic<bool> s_running{false};
std::atomic<int> s_streams{0}, s_requests{0};
std::atomic<int> s_loops{0};  // the two loops run detached, so no thread object can outlive the process and end it in std::terminate
SOCKET s_listen = INVALID_SOCKET, s_dns = INVALID_SOCKET;
std::string s_pin;  // under s_lock
unsigned short s_port = 0;
std::mutex s_lock;
// Wrong tries and the lockout are kept per phone address, so a stale tab on one phone never locks out another.
struct Tries { int failures = 0; std::chrono::steady_clock::time_point lockedUntil; };
std::map<unsigned long, Tries> s_tries;

bool SamePin(const std::string& given) {
    unsigned diff = static_cast<unsigned>(given.size() ^ s_pin.size());
    for (std::size_t i = 0; i < given.size() && i < s_pin.size(); ++i) diff |= static_cast<unsigned char>(given[i] ^ s_pin[i]);
    return diff == 0;
}
// 0 ok, 1 wrong, 2 locked out. Five wrong tries lock that phone out for 30 seconds.
int CheckPin(const std::string& given, unsigned long peer) {
    std::lock_guard<std::mutex> hold(s_lock);
    const auto now = std::chrono::steady_clock::now();
    auto& tries = s_tries[peer];
    if (now < tries.lockedUntil) return 2;
    if (SamePin(given)) { tries.failures = 0; return 0; }
    if (++tries.failures >= FailuresAllowed) { tries.failures = 0; tries.lockedUntil = now + std::chrono::seconds(LockSeconds); }
    return 1;
}
void Reply(SOCKET socket, int status, const char* type, const std::string& body, const std::string& extra = {}) {
    const char* reason = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 403 ? "Forbidden" : status == 429 ? "Too Many Requests" : status == 500 ? "Internal Server Error" : "Not Found";
    const std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " + type +
        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nCache-Control: no-store\r\n" + extra + "Connection: close\r\n\r\n";
    send(socket, head.data(), static_cast<int>(head.size()), 0);
    send(socket, body.data(), static_cast<int>(body.size()), 0);
}
std::string HeaderValue(const std::string& request, const char* name) {
    const std::string key = std::string("\r\n") + name + ":";
    auto at = request.find(key);
    if (at == std::string::npos) return {};
    at += key.size();
    const auto end = request.find("\r\n", at);
    auto value = request.substr(at, end == std::string::npos ? std::string::npos : end - at);
    value.erase(0, value.find_first_not_of(' '));
    return value;
}
std::string Words(const char* key, int arg) { return arg < 0 ? std::string(loc::T(key)) : loc::Tf(key, arg); }
// One phone's live stream: what changed since the last message, about 20 times a second. It ends when the
// phone goes or the server stops. ponytail: each stream reads the view itself; one shared reader if phones multiply.
void Stream(SOCKET socket) {
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
        if (const auto now = readings.dump(); now != data) { data = now; message["data"] = readings; }
        if (const auto now = extras.dump(); now != live) { live = now; message["live"] = extras; }
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
    closesocket(socket);
    --s_streams;
}
// Ember's frame data (assets/framedata/usf4.json), embedded in this binary (FrameDataTable.hxx), never a package
// file: an older updater refuses a package file it does not know. Parsed once; null when it is bad.
const nlohmann::json& FrameData() {
    static const nlohmann::json data = nlohmann::json::parse(frameDataTable, frameDataTable + sizeof(frameDataTable) - 1, nullptr, false);
    return data;
}
std::string UrlDecode(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) && std::isxdigit(static_cast<unsigned char>(text[i + 2]))) { out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16)); i += 2; }
        else out += text[i] == '+' ? ' ' : text[i];
    }
    return out;
}
std::string FighterName(int id) { const auto* fighter = selection::FindFighter(id); return fighter ? fighter->name : std::string(); }
// A combo order from the phone: save the last recorded one, rename, delete or play a saved one.
nlohmann::json ComboOrder(const nlohmann::json& body, const std::string& cmd) {
    const auto fail = [](const char* why) { return nlohmann::json{{"ok", false}, {"error", why}}; };
    auto& store = Combos();
    const auto id = body.value("id", nlohmann::json());
    std::string name, error;
    if (cmd == "comboSave") {
        const auto last = LastCombo();
        if (last.frames.empty()) return fail("no combo recorded");
        if (!ParseComboName(body.value("name", nlohmann::json()), name, error)) return fail("bad name");
        SavedCombo combo; combo.name = name; combo.fighter = last.fighter; combo.hits = (std::min)(last.hits, 999);
        combo.x[0] = last.x[0]; combo.x[1] = last.x[1]; combo.frames = last.frames;
        std::string saved;
        return store.Add(std::move(combo), saved, error) ? nlohmann::json{{"ok", true}, {"id", saved}} : fail("could not save");
    }
    if (!id.is_string()) return fail("no id");
    if (cmd == "comboDelete") return {{"ok", store.Delete(id.get<std::string>())}};
    if (cmd == "comboRename") {
        if (!ParseComboName(body.value("name", nlohmann::json()), name, error)) return fail("bad name");
        return store.Rename(id.get<std::string>(), name, error) ? nlohmann::json{{"ok", true}} : fail("could not rename");
    }
    SavedCombo combo;
    const auto view = ReadView();
    if (!view.available || !store.Load(id.get<std::string>(), combo)) return fail("cannot play");
    bool ok = true;
    for (auto& command : ComboPlayCommands(combo, view.generation)) ok &= Submit(std::move(command));
    return {{"ok", ok}};
}
// How large a request body may be: only a combo brought in and a HUD layout or picture may be big.
std::size_t MostBody(const std::string& request) {
    static const struct { const char* start; std::size_t most; } limits[] = {
        { "POST /api/combos ", ComboFileBytes }, { "POST /api/hud/image ", HudImageBytes }, { "POST /api/hud/", HudBytes } };
    for (const auto& limit : limits) if (request.compare(0, std::strlen(limit.start), limit.start) == 0) return limit.most;
    return 1024;
}
// Answers one short request; a stream takes the socket over and returns true.
bool Serve(SOCKET socket, unsigned long peer) {
    DWORD timeout = 2000; setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::string request; char buffer[2048];
    for (;;) {
        const auto end = request.find("\r\n\r\n");
        if (end != std::string::npos) {
            const std::size_t most = MostBody(request);
            const auto length = std::atoi(HeaderValue(request, "Content-Length").c_str());
            if (length < 0 || static_cast<std::size_t>(length) > most) { Reply(socket, 400, "text/plain", "too big"); return false; }
            if (request.size() >= end + 4 + static_cast<std::size_t>(length)) break;
        }
        if (request.size() > 8192 + MostBody(request)) { Reply(socket, 400, "text/plain", "too big"); return false; }
        const int got = recv(socket, buffer, sizeof(buffer), 0);
        if (got <= 0) return false;
        request.append(buffer, got);
    }
    const bool post = request.compare(0, 5, "POST ") == 0, get = request.compare(0, 4, "GET ") == 0;
    const auto target = request.substr(get ? 4 : 5, request.find(' ', 5) - (get ? 4 : 5));
    const std::size_t query = target.find('?');
    const std::string path = target.substr(0, query);
    if (get && path == "/") { static const std::string page = PageWithHudEditor(); Reply(socket, 200, "text/html; charset=utf-8", page); return false; }
    const bool download = get && path.compare(0, 12, "/api/combos/") == 0;
    if (path != "/api/options" && path != "/api/state" && path != "/api/cmd" && path != "/api/stream" && path != "/api/combos" && path != "/api/frames" && path != "/api/camera" && !download && !hud::Owns(path)) { Reply(socket, 404, "text/plain", "no"); return false; }
    // An EventSource and a download link cannot set headers, so those take the PIN in their address.
    std::string given = HeaderValue(request, "X-Pin");
    if ((path == "/api/stream" || download) && query != std::string::npos && target.compare(query, 5, "?pin=") == 0) given = target.substr(query + 5);
    if (const int pin = CheckPin(given, peer)) { Reply(socket, pin == 2 ? 429 : 403, "text/plain", "pin"); return false; }
    if (hud::Owns(path)) {
        static const hud::Host host{ [] { return sf4e::GameThemes::Directory(); }, [](const std::string& name, std::map<std::string, std::string> files) { sf4e::GameThemes::ApplyHud(name, std::move(files)); }, [] { sf4e::GameThemes::Restart(); } };
        const auto reply = hud::Handle(host, post, path, query == std::string::npos ? std::string() : target.substr(query + 1), request.substr(request.find("\r\n\r\n") + 4), HeaderValue(request, "User-Agent"));
        Reply(socket, reply.status, reply.type.c_str(), reply.body);
        return false;
    }
    if (get && path == "/api/options") { Reply(socket, 200, "application/json", OptionsJson(Words).dump()); return false; }
    if (get && path == "/api/state") { Reply(socket, 200, "application/json", ValuesJson(ReadView(), Phone().Values()).dump()); return false; }
    if (get && path == "/api/camera") { Reply(socket, 200, "application/json", camera::Phone().State().dump()); return false; }
    if (post && path == "/api/camera") {
        std::string problem;
        const auto answer = camera::Phone().Order(nlohmann::json::parse(request.substr(request.find("\r\n\r\n") + 4), nullptr, false), problem);
        if (answer.is_null()) Reply(socket, 400, "application/json", nlohmann::json{{"ok", false}, {"error", problem}}.dump());
        else Reply(socket, 200, "application/json", answer.dump());
        return false;
    }
    if (get && path == "/api/combos") { Reply(socket, 200, "application/json", CombosJson(Combos().List(), FighterName).dump()); return false; }
    if (get && path == "/api/frames") {
        // ?fighter=Name gives that fighter's rows; none, the fighters.
        const auto at = query == std::string::npos ? std::string::npos : target.find("fighter=", query);
        const auto reply = at == std::string::npos ? FramesIndex(FrameData()) : FramesOf(FrameData(), UrlDecode(target.substr(at + 8, target.find('&', at) - at - 8)));
        if (reply.is_null()) Reply(socket, 404, "text/plain", "no"); else Reply(socket, 200, "application/json", reply.dump());
        return false;
    }
    if (download) {
        std::string bytes; const auto id = path.substr(12);
        if (!Combos().Bytes(id, bytes)) { Reply(socket, 404, "text/plain", "no"); return false; }
        Reply(socket, 200, "application/json", bytes, "Content-Disposition: attachment; filename=\"ember-combo-" + id + ".json\"\r\n");
        return false;
    }
    if (post && path == "/api/combos") {
        std::string id, error;
        const bool ok = Combos().Import(request.substr(request.find("\r\n\r\n") + 4), id, error);
        Reply(socket, ok ? 200 : 400, "application/json", nlohmann::json{{"ok", ok}, {"id", id}, {"error", error}}.dump());
        return false;
    }
    if (get && path == "/api/stream") {
        if (s_streams >= MaxStreams) { Reply(socket, 429, "text/plain", "streams"); return false; }
        ++s_streams;
        std::thread(Stream, socket).detach();
        return true;
    }
    if (!post || path != "/api/cmd") { Reply(socket, 404, "text/plain", "no"); return false; }
    const auto body = nlohmann::json::parse(request.substr(request.find("\r\n\r\n") + 4), nullptr, false);
    const auto cmd = body.is_object() ? body.value("cmd", nlohmann::json()) : nlohmann::json();
    if (cmd.is_string() && cmd.get<std::string>().compare(0, 5, "combo") == 0 && cmd != "comboRecord" && cmd != "comboStop" && cmd != "comboStart") {
        Reply(socket, 200, "application/json", ComboOrder(body, cmd.get<std::string>()).dump());
        return false;
    }
    const auto view = ReadView();
    Order order; std::string error;
    const auto route = ParseOrder(body, view, order, error);
    if (route == Route::Refused) { Reply(socket, 400, "application/json", nlohmann::json{{"ok", false}, {"error", error}}.dump()); return false; }
    const bool ok = route == Route::Game ? Submit(std::move(order.command)) : order.id == "replyMoves" ? Phone().PushText(order.id, order.text) : Phone().Push(order.id, order.value);
    Reply(socket, 200, "application/json", nlohmann::json{{"ok", ok}}.dump());
    return false;
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
        // A thread each: a phone's browser opens idle spare connections, and one waiting for its request must not hold up the rest.
        if (!PrivatePeer(from) || s_requests >= MaxRequests) { closesocket(socket); continue; }
        ++s_requests;
        std::thread([socket, from] { if (!Serve(socket, from)) closesocket(socket); --s_requests; }).detach();
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
    WSADATA data; if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    unsigned char random[4]; unsigned number = 0;
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
    std::memcpy(&number, random, 4);
    char pin[8]; std::snprintf(pin, sizeof(pin), "%06u", number % 1000000);
    { std::lock_guard<std::mutex> hold(s_lock); s_pin = pin; s_tries.clear(); }
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
    while (s_loops > 0 || s_streams > 0 || s_requests > 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));  // the streams see s_running and end
    closesocket(s_listen); if (s_dns != INVALID_SOCKET) closesocket(s_dns);
    s_listen = s_dns = INVALID_SOCKET; s_port = 0;
    std::lock_guard<std::mutex> hold(s_lock); s_pin.clear();
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
std::string Pin() { std::lock_guard<std::mutex> hold(s_lock); return s_running ? s_pin : std::string(); }
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
