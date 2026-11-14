#pragma once
#include "ComboFile.hxx"
#include "RemoteHud.hxx"
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

// How the phone remote (RemoteServer.cxx) takes a request: every route it
// answers in one table, the request's head read and checked against it, the
// PIN, and the limits that keep what requests hold small and short. A request
// is refused on its head alone, before any of its body is read. Pure: no
// sockets and no game.
namespace sf4e { namespace training { namespace remote {
using Clock = std::chrono::steady_clock;
enum class Method { Get, Post };
// What answers a route: the server's own (RemoteServer.cxx), or one of the HUD editor's orders (RemoteHud.cxx).
enum class Endpoint { Page, Options, State, Stream, Command, Camera, CameraOrder, Combos, ComboImport, ComboFile, Frames };
using Handler = std::variant<Endpoint, hud::Order>;
// Where a request's PIN is: none asked (the page itself), the X-Pin header, or also the address (?pin=), since
// an EventSource and a download link cannot set a header.
enum class PinFrom { None, Header, HeaderOrAddress };
// What must hold for a route to be served now: nothing more, or the camera in use (Training, a watched replay,
// a shot rendering).
enum class Needs { Nothing, Camera };
// The bytes a request may bring: its head (the request line and headers) on every route, and each route's body.
constexpr std::size_t HeadBytes = 8192, CommandBytes = 1024, CameraBytes = 16384, HudBytes = 64u << 10, HudImageBytes = 6u << 20;
// The bodies all requests under way may hold together: one HUD picture and the small ones beside it. With at
// most 32 requests of an 8 KB head each, requests never hold more than about 8.3 MB.
constexpr std::size_t BodiesHeld = 8u << 20;
// How long a request may take to arrive: its head, and all of it.
constexpr std::chrono::seconds HeadTime{5}, RequestTime{30};
struct Resource { Method method; const char* path; bool prefix; std::size_t body; PinFrom pin; Needs needs; Handler handler; };
// The route a request's method and path name, or none. A prefix route takes what follows its path (a combo's id).
inline const Resource* FindResource(Method method, const std::string& path) {
    static const Resource table[] = {
        {Method::Get, "/", false, 0, PinFrom::None, Needs::Nothing, Endpoint::Page},
        {Method::Get, "/api/options", false, 0, PinFrom::Header, Needs::Nothing, Endpoint::Options},
        {Method::Get, "/api/state", false, 0, PinFrom::Header, Needs::Nothing, Endpoint::State},
        {Method::Get, "/api/stream", false, 0, PinFrom::HeaderOrAddress, Needs::Nothing, Endpoint::Stream},
        {Method::Post, "/api/cmd", false, CommandBytes, PinFrom::Header, Needs::Nothing, Endpoint::Command},
        {Method::Get, "/api/camera", false, 0, PinFrom::Header, Needs::Nothing, Endpoint::Camera},
        {Method::Post, "/api/camera", false, CameraBytes, PinFrom::Header, Needs::Camera, Endpoint::CameraOrder},
        {Method::Get, "/api/combos", false, 0, PinFrom::Header, Needs::Nothing, Endpoint::Combos},
        {Method::Post, "/api/combos", false, ComboFileBytes, PinFrom::Header, Needs::Nothing, Endpoint::ComboImport},
        {Method::Get, "/api/combos/", true, 0, PinFrom::HeaderOrAddress, Needs::Nothing, Endpoint::ComboFile},
        {Method::Get, "/api/frames", false, 0, PinFrom::Header, Needs::Nothing, Endpoint::Frames},
        {Method::Get, "/api/hud/themes", false, 0, PinFrom::Header, Needs::Nothing, hud::Order::Themes},
        {Method::Get, "/api/hud/theme", false, 0, PinFrom::Header, Needs::Nothing, hud::Order::Theme},
        {Method::Get, "/api/hud/scene", false, 0, PinFrom::Header, Needs::Nothing, hud::Order::Scene},
        {Method::Get, "/api/hud/sheet", false, 0, PinFrom::Header, Needs::Nothing, hud::Order::Sheet},
        {Method::Post, "/api/hud/image", false, HudImageBytes, PinFrom::Header, Needs::Nothing, hud::Order::Image},
        {Method::Post, "/api/hud/image-remove", false, HudBytes, PinFrom::Header, Needs::Nothing, hud::Order::ImageRemove},
        {Method::Post, "/api/hud/restart", false, HudBytes, PinFrom::Header, Needs::Nothing, hud::Order::Restart},
        {Method::Post, "/api/hud/save-as", false, HudBytes, PinFrom::Header, Needs::Nothing, hud::Order::SaveAs},
        {Method::Post, "/api/hud/layout", false, HudBytes, PinFrom::Header, Needs::Nothing, hud::Order::Layout},
        {Method::Post, "/api/hud/apply", false, HudBytes, PinFrom::Header, Needs::Nothing, hud::Order::Apply},
    };
    for (const Resource& resource : table) {
        if (resource.method != method) continue;
        const std::size_t length = std::strlen(resource.path);
        if (resource.prefix ? path.size() > length && path.compare(0, length, resource.path) == 0 : path == resource.path) return &resource;
    }
    return nullptr;
}

// %XX and '+' in an address's query.
inline std::string UrlDecode(const std::string& text) {
    const auto hex = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : std::tolower(static_cast<unsigned char>(c)) - 'a' + 10; };
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) && std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2])); i += 2;
        }
        else out += text[i] == '+' ? ' ' : text[i];
    }
    return out;
}
// One value of a query (what follows the '?'), decoded; empty when it has none.
inline std::string QueryParam(const std::string& query, const std::string& key) {
    for (std::size_t at = 0; at < query.size();) {
        const auto end = query.find('&', at), stop = end == std::string::npos ? query.size() : end;
        if (query.compare(at, key.size() + 1, key + "=") == 0) return UrlDecode(query.substr(at + key.size() + 1, stop - at - key.size() - 1));
        at = stop + 1;
    }
    return {};
}

// What a request's head says: the method, the path and the query, and the headers the remote reads.
struct Head { Method method = Method::Get; std::string path, query; std::size_t length = 0; std::string pin, userAgent; };
// Reads a head (everything before the blank line). False for anything the remote does not take: another
// method, a request line it cannot read, or a Content-Length that is not a plain number below 1 GB.
inline bool ParseHead(const std::string& head, Head& out) {
    out = Head{};
    const auto lineEnd = (std::min)(head.find("\r\n"), head.size());
    const std::string line = head.substr(0, lineEnd);
    std::size_t at;
    if (line.compare(0, 4, "GET ") == 0) { out.method = Method::Get; at = 4; }
    else if (line.compare(0, 5, "POST ") == 0) { out.method = Method::Post; at = 5; }
    else return false;
    const auto space = line.find(' ', at);
    if (space == std::string::npos || space == at || line.compare(space, 6, " HTTP/") != 0) return false;
    const std::string target = line.substr(at, space - at);
    const auto query = target.find('?');
    out.path = target.substr(0, query);
    out.query = query == std::string::npos ? std::string() : target.substr(query + 1);
    bool lengthSeen = false;
    for (std::size_t from = lineEnd + 2; from < head.size();) {
        const auto end = (std::min)(head.find("\r\n", from), head.size());
        const std::string header = head.substr(from, end - from);
        from = end + 2;
        const auto colon = header.find(':');
        if (colon == std::string::npos) continue;
        std::string name = header.substr(0, colon), value = header.substr(colon + 1);
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        if (name == "content-length") {
            if (lengthSeen || value.empty() || value.size() > 9 || value.find_first_not_of("0123456789") != std::string::npos) return false;
            out.length = static_cast<std::size_t>(std::stoul(value)); lengthSeen = true;
        }
        else if (name == "x-pin") out.pin = value;
        else if (name == "user-agent") out.userAgent = value;
    }
    return true;
}

// The PIN and the wrong tries against it. Five wrong tries lock that address out for 30 seconds, so a stale
// tab on one phone never locks out another. At most MaxPeers addresses are kept. Past Budget wrong tries in a
// minute from all of them, only an address that has given the right PIN gets in until the minute is over.
class PinGate {
public:
    enum class Verdict { Right, Wrong, Locked };
    static constexpr int FailuresAllowed = 5, Budget = 30;
    static constexpr std::size_t MaxPeers = 64;
    static constexpr std::chrono::seconds LockFor{30}, BudgetWindow{60};
    // A new PIN: every address starts over.
    void Reset(std::string pin) { std::lock_guard<std::mutex> hold(lock_); pin_ = std::move(pin); peers_.clear(); wrong_ = 0; window_ = Clock::time_point{}; }
    std::string Pin() const { std::lock_guard<std::mutex> hold(lock_); return pin_; }
    Verdict Check(const std::string& given, unsigned long peer, Clock::time_point now) {
        std::lock_guard<std::mutex> hold(lock_);
        if (window_ == Clock::time_point{} || now - window_ >= BudgetWindow) { window_ = now; wrong_ = 0; }
        auto found = peers_.find(peer);
        if (found == peers_.end()) {
            // Addresses with nothing to keep make room: not locked out and never signed in.
            for (auto it = peers_.begin(); peers_.size() >= MaxPeers && it != peers_.end();)
                it = !it->second.signedIn && now >= it->second.lockedUntil ? peers_.erase(it) : std::next(it);
            if (peers_.size() >= MaxPeers) return Verdict::Locked;
            found = peers_.emplace(peer, Peer{}).first;
        }
        Peer& p = found->second;
        if (now < p.lockedUntil || (wrong_ >= Budget && !p.signedIn)) return Verdict::Locked;
        if (Same(given)) { p.failures = 0; p.signedIn = true; return Verdict::Right; }
        ++wrong_;
        if (++p.failures >= FailuresAllowed) { p.failures = 0; p.lockedUntil = now + LockFor; }
        return Verdict::Wrong;
    }
private:
    struct Peer { int failures = 0; Clock::time_point lockedUntil; bool signedIn = false; };
    // Compares every byte whatever differs, so the time taken says nothing about the PIN. An empty PIN (stopped) matches nothing.
    bool Same(const std::string& given) const {
        unsigned diff = static_cast<unsigned>(given.size() ^ pin_.size()) | (pin_.empty() ? 1u : 0u);
        for (std::size_t i = 0; i < given.size() && i < pin_.size(); ++i) diff |= static_cast<unsigned char>(given[i] ^ pin_[i]);
        return diff == 0;
    }
    mutable std::mutex lock_;
    std::string pin_;
    std::map<unsigned long, Peer> peers_;
    int wrong_ = 0;
    Clock::time_point window_;
};

// The body bytes all requests under way hold together, so a busy remote holds little whatever its clients send.
class BodyBudget {
public:
    explicit BodyBudget(std::size_t most) : most_(most) {}
    bool Take(std::size_t bytes) {
        std::size_t held = held_.load();
        do { if (bytes > most_ - held) return false; } while (!held_.compare_exchange_weak(held, held + bytes));
        return true;
    }
    void Give(std::size_t bytes) { held_ -= bytes; }
    std::size_t Held() const { return held_; }
    // One request's share, given back when it goes; false when it did not fit.
    class Lease {
    public:
        Lease(BodyBudget& budget, std::size_t bytes) : budget_(budget), bytes_(bytes), held_(budget.Take(bytes)) {}
        ~Lease() { if (held_) budget_.Give(bytes_); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        explicit operator bool() const { return held_; }
    private:
        BodyBudget& budget_; std::size_t bytes_; bool held_;
    };
private:
    const std::size_t most_;
    std::atomic<std::size_t> held_{0};
};

// How long the next wait for a client's bytes may be: what is left before `deadline`, in slices short enough
// that a stopping server is seen at once; zero once the time is up.
constexpr std::chrono::milliseconds WaitSlice{200};
inline std::chrono::milliseconds NextWait(Clock::time_point now, Clock::time_point deadline) {
    if (now >= deadline) return std::chrono::milliseconds(0);
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return left < WaitSlice ? left : WaitSlice;
}

inline Reply TextReply(int status, const char* text) { Reply reply; reply.status = status; reply.type = "text/plain"; reply.body = text; return reply; }
inline Reply JsonReply(const nlohmann::json& value, int status = 200) { Reply reply; reply.status = status; reply.body = value.dump(); return reply; }
// A route that cannot be served now, as the page shows it.
inline const char* NotNow(Needs needs) { return needs == Needs::Camera ? "the camera works in Training and in replays" : ""; }
// A request decided on its head alone: refused, with the answer to send, or taken by its route. In this order:
// an address the remote does not answer (404), a wrong PIN or one locked out (403, 429), a route that cannot be
// served now (409), a body over the route's limit (400). `holds` says whether what a route needs holds now.
struct Admission { const Resource* resource = nullptr; std::optional<Reply> refused; };
inline Admission Admit(const Head& head, PinGate& pins, unsigned long peer, Clock::time_point now, const std::function<bool(Needs)>& holds) {
    Admission admission;
    const Resource* resource = FindResource(head.method, head.path);
    if (!resource) { admission.refused = TextReply(404, "no"); return admission; }
    if (resource->pin != PinFrom::None) {
        const std::string inAddress = resource->pin == PinFrom::HeaderOrAddress ? QueryParam(head.query, "pin") : std::string();
        const auto verdict = pins.Check(inAddress.empty() ? head.pin : inAddress, peer, now);
        if (verdict != PinGate::Verdict::Right) { admission.refused = TextReply(verdict == PinGate::Verdict::Locked ? 429 : 403, "pin"); return admission; }
    }
    if (!holds(resource->needs)) { admission.refused = JsonReply({{"ok", false}, {"error", NotNow(resource->needs)}}, 409); return admission; }
    if (head.length > resource->body) { admission.refused = TextReply(400, "too big"); return admission; }
    admission.resource = resource;
    return admission;
}
} } }
