#pragma once
#include "../common/EndpointIdentity.hxx"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace sf4e { namespace netplay {
struct LanRoom {
    std::string endpoint, name, build;
    unsigned members=0, capacity=0;
};
inline std::string LanInvitation(const LanRoom& room) { return "sf4lan:"+room.endpoint; }
struct LanRooms {
    std::string status="searching";
    std::vector<LanRoom> rooms;
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
    return result;
}
} }
