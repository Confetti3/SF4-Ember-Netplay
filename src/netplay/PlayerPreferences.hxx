#pragma once
#include <string>
#include "../common/FighterCatalog.hxx"
#include "../common/RoomRules.hxx"
#include "ProfileRecord.hxx"

namespace sf4e { namespace netplay {

struct LobbySettings {
    bool editionSelect = true;
    int roundCount = 3;
    int roundTime = 99;
    bool operator==(const LobbySettings& other) const {
        return editionSelect == other.editionSelect && roundCount == other.roundCount && roundTime == other.roundTime;
    }
    bool Valid() const {
        const bool rounds = roundCount == 1 || roundCount == 3 || roundCount == 5 || roundCount == 7 || roundCount == 15 || roundCount == 99;
        const bool time = roundTime == 30 || roundTime == 60 || roundTime == 99 || roundTime == 300 || roundTime == 9999;
        return rounds && time;
    }
};

struct PlayerPreferences {
    std::string displayName = "Player";
    int mainFighter = 0;
    ProfileRecord record;
    int inputDelay = 2;
    // On by default so a stall or rollback spike is visible, but Small: the standard strip drew the eye mid-fight.
    bool showMatchHud = true;
    int matchHudSize = 0;
    bool matchHudRaised = false;
    bool discordPresence = true, discordInvites = true;
    float interfaceScale = 1.f;
    LobbySettings lobby;
    std::string roomName = "Private room";
    int roomCapacity = 16;
    room::Rules tableRules;
    bool Valid() const {
        if (displayName.empty() || displayName.size() >= 32 || mainFighter<0 || mainFighter>=selection::FighterCount || inputDelay < 0 || inputDelay > 10 ||
            matchHudSize < 0 || matchHudSize > 2 || !(interfaceScale >= 1.f && interfaceScale <= 1.5f) || !lobby.Valid() ||
            roomName.empty() || roomName.size() > 64 || roomCapacity < 2 || roomCapacity > static_cast<int>(room::MaxMembers)) return false;
        for (unsigned char c : displayName) if (c < 32 || c == 127) return false;
        for (unsigned char c : roomName) if (c < 32 || c == 127) return false;
        if (tableRules.format != room::SetFormat::Unlimited || tableRules.rotation != room::RotationMode::WinnerStays) return false;
        LobbySettings battle;
        battle.editionSelect = tableRules.editionSelect;
        battle.roundCount = tableRules.roundCount;
        battle.roundTime = tableRules.roundTime;
        if (!battle.Valid()) return false;
        return true;
    }
};

} }
