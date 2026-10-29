#pragma once
#include "../common/EndpointIdentity.hxx"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <memory>

namespace sf4e { namespace netplay {
struct LanRoom {
    std::string endpoint, name, build;
    unsigned members=0, capacity=0;
};
struct LanRooms {
    std::string status="searching";
    std::vector<LanRoom> rooms;
};
// Keyless browser lifetime: the LAN browser is not a member of any room.
class LanBrowserLease {
public:
    static constexpr std::uint64_t RenewMs = 5000, ExpiryMs = 15000;
    std::uint64_t Active() const { return active_; }
    std::uint64_t Due(std::uint64_t now) { return !active_ ? ++next_ : now - sent_ >= RenewMs ? active_ : 0; }
    void Sent(std::uint64_t view, std::uint64_t now) { active_ = view; sent_ = now; }
    void Stop() { active_ = 0; }
private:
    std::uint64_t next_ = 0, active_ = 0, sent_ = 0;
};
class LanBrowserPulse {
public:
    static constexpr std::uint64_t ExpiryMs = 1500;
    void Note(std::uint64_t now) { at_.store(now, std::memory_order_release); }
    bool Shown(std::uint64_t now) const {
        const auto at = at_.load(std::memory_order_acquire);
        return at && now >= at && now - at < ExpiryMs;
    }
private:
    std::atomic<std::uint64_t> at_{0};
};
// Discovery metadata is untrusted. Admission still checks the chosen endpoint,
// exact build and room capability; a display name grants no authority.
inline LanRooms ReadLanRooms(const nlohmann::json& event) {
    LanRooms result;
    result.status=event.at("status").get<std::string>();
    if(result.status!="ready"&&result.status!="unavailable")throw nlohmann::json::type_error::create(302,"invalid LAN status",&event);
    const auto& rows=event.at("rooms");
    if(!rows.is_array()||rows.size()>64)throw nlohmann::json::type_error::create(302,"invalid LAN room count",&event);
    for(const auto& row:rows){
        LanRoom r;
        r.endpoint=row.at("endpoint").get<std::string>();r.name=row.at("n").get<std::string>();r.build=row.at("b").get<std::string>();
        r.members=row.at("m").get<unsigned>();r.capacity=row.at("c").get<unsigned>();
        if(!IsEndpointIdentity(r.endpoint)||r.name.empty()||r.name.size()>64||r.build.empty()||r.build.size()>128||r.capacity<2||r.capacity>16||r.members<1||r.members>r.capacity)
            throw nlohmann::json::type_error::create(302,"invalid LAN room",&event);
        for(const unsigned char c:r.name)if(c<32||c==127)throw nlohmann::json::type_error::create(302,"invalid LAN room name",&event);
        result.rooms.push_back(std::move(r));
    }
    std::sort(result.rooms.begin(), result.rooms.end(), [](const auto& a, const auto& b) {
        return a.name == b.name ? a.endpoint < b.endpoint : a.name < b.name;
    });
    return result;
}
} }
