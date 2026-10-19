#pragma once
#include <nlohmann/json.hpp>
namespace sf4e { namespace netplay {
enum class RoomTransport { Online, RelayOnly, Lan };
inline RoomTransport ReadRoomTransport(const nlohmann::json& event) {
    const auto policy=event.at("transport").get<std::string>();
    if(policy=="online")return RoomTransport::Online;
    if(policy=="relay_only")return RoomTransport::RelayOnly;
    if(policy=="lan")return RoomTransport::Lan;
    throw nlohmann::json::type_error::create(302,"invalid room transport",&event);
}
} }
