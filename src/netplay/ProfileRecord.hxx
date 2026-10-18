#pragma once
#include "../common/RoomLimits.hxx"
#include "../common/MatchResult.hxx"
#include <deque>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

namespace sf4e { namespace netplay {
using RandomRoomId = std::array<std::uint8_t, 16>;

inline bool HasRandomRoomId(const RandomRoomId& roomId) {
    return std::any_of(roomId.begin(), roomId.end(), [](std::uint8_t byte) { return byte != 0; });
}

// The room's random 128-bit identity is stable for that room instance while
// its process epoch can be reused by another room.  Keep the version marker
// in the persisted key so future formats can coexist with both this format
// and legacy epoch-based strings already stored in `recent`.
inline std::string ProfileResultKeyV2(const RandomRoomId& roomId, unsigned table, std::uint64_t generation) {
    static constexpr char Hex[] = "0123456789abcdef";
    std::string key = "v2:";
    key.reserve(64);
    for (const auto byte : roomId) {
        key.push_back(Hex[(byte >> 4) & 0xf]);
        key.push_back(Hex[byte & 0xf]);
    }
    key += ":" + std::to_string(table) + ":" + std::to_string(generation);
    return key;
}

// Local confirmed online record, not a Steam rank or historical-game import.
inline std::string ProfileResultKeyV3(const RandomRoomId& roomId, unsigned table,
    std::uint64_t generation, std::uint64_t game) {
    // Base 36 keeps both full 64-bit counters inside the existing 64-byte
    // key budget. The room ID remains the same 128-bit hexadecimal identity.
    const auto base36 = [](std::uint64_t value) {
        std::string result;
        do { result.push_back("0123456789abcdefghijklmnopqrstuvwxyz"[value % 36]); value /= 36; } while (value);
        std::reverse(result.begin(), result.end()); return result;
    };
    auto key = ProfileResultKeyV2(roomId, table, 1);
    static_assert(room::TableCount <= 10, "One digit table in bounded continuous profile keys");
    key[1] = '3'; key.erase(2, 1); key.resize(key.size() - 1);
    return key + base36(generation) + ":" + base36(game);
}

struct ProfileRecord {
    static constexpr std::uint64_t MaximumGames = 1000000000;
    static constexpr std::size_t MaximumRecentEntries = 256;
    static constexpr std::size_t MaximumRecentKeyBytes = 64;
    std::uint64_t wins=0,losses=0;
    bool available=true;
    std::deque<std::string> recent;
    bool Record(const RandomRoomId& roomId,unsigned table,std::uint64_t generation,unsigned localSlot,room::MatchResult result) {
        if(!available||!HasRandomRoomId(roomId)||!generation||table>=room::TableCount||localSlot>=2||
            (result!=room::MatchResult::P1Win&&result!=room::MatchResult::P2Win))return false;
        const auto key=ProfileResultKeyV2(roomId,table,generation);
        return RecordKey(key, localSlot, result);
    }
    bool RecordContinuous(const RandomRoomId& roomId,unsigned table,std::uint64_t generation,
        std::uint64_t game,unsigned localSlot,room::MatchResult result) {
        if(!available||!HasRandomRoomId(roomId)||!generation||!game||table>=room::TableCount||localSlot>=2||
            (result!=room::MatchResult::P1Win&&result!=room::MatchResult::P2Win))return false;
        return RecordKey(ProfileResultKeyV3(roomId,table,generation,game),localSlot,result);
    }
private:
    bool RecordKey(const std::string& key,unsigned localSlot,room::MatchResult result) {
        if(key.size()>MaximumRecentKeyBytes)return false;
        if(std::find(recent.begin(),recent.end(),key)!=recent.end())return false;
        // The persisted counters describe a bounded total history.  Reject
        // the exact limit as well as values past it so one more result can
        // never overflow the accounting contract.
        if(wins>=MaximumGames||losses>=MaximumGames||losses>=MaximumGames-wins)return false;
        const bool won=(result==room::MatchResult::P1Win)==(localSlot==0);
        if(won)++wins;else ++losses;
        recent.push_back(key);if(recent.size()>MaximumRecentEntries)recent.pop_front();return true;
    }
};
} }
