#pragma once
#include <nlohmann/json.hpp>
namespace sf4e { namespace netplay {
enum class RoomKind { Online, Lan };
inline const char* RoomKindName(RoomKind kind) { return kind==RoomKind::Lan?"lan":"online"; }
enum class RoomTransport { Online, RelayOnly, Lan };
inline RoomKind KindOfTransport(RoomTransport policy) { return policy==RoomTransport::Lan?RoomKind::Lan:RoomKind::Online; }
inline const char* TransportToString(RoomTransport policy) { return policy==RoomTransport::Lan?"lan":policy==RoomTransport::RelayOnly?"relay_only":"online"; }
inline RoomTransport ReadRoomTransport(const nlohmann::json& event) {
    const auto policy=event.at("transport").get<std::string>();
    if(policy=="online")return RoomTransport::Online;
    if(policy=="relay_only")return RoomTransport::RelayOnly;
    if(policy=="lan")return RoomTransport::Lan;
    throw nlohmann::json::type_error::create(302,"invalid room transport",&event);
}
} }
