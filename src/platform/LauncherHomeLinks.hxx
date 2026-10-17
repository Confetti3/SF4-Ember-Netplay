#pragma once
#include "JoinLinkMailbox.hxx"
#include <memory>
namespace sf4e { namespace platform {
inline const wchar_t* DiscordLaunchSectionName() { return L"Local\\SF4EmberDiscordLaunch"; }
inline const wchar_t* DiscordLaunchEventName() { return L"Local\\SF4EmberDiscordLaunchReady"; }
struct DiscordLaunchSlot { unsigned launch = 0; };
inline bool DeliverDiscordLaunch(const wchar_t* section=DiscordLaunchSectionName(),const wchar_t* ready=DiscordLaunchEventName()) {
    return DeliverSlot(DiscordLaunchSlot{1},section,ready);
}
struct HomeLaunchLinks {
    std::string join,connect,replay;
    tournament_link::MatchLink match;
    tournament_link::RoomLink room;
    bool discord=false;
    bool Any() const { return !join.empty()||match.Valid()||room.Valid()||!connect.empty()||!replay.empty()||discord; }
};
// Own these only while Home has the launcher lock but no game. Destruction
// closes every mapping before the game opens its own receiver. Tests supply
// a private suffix and use the same validators and kernel mailbox primitive.
class LauncherHomeLinks {
public:
    explicit LauncherHomeLinks(const std::wstring& scope=L"")
      : join_((std::wstring(JoinLinkSectionName())+scope).c_str(),(std::wstring(JoinLinkEventName())+scope).c_str()),
        match_((std::wstring(MatchLinkSectionName())+scope).c_str(),(std::wstring(MatchLinkEventName())+scope).c_str()),
        room_((std::wstring(PublicRoomLinkSectionName())+scope).c_str(),(std::wstring(PublicRoomLinkEventName())+scope).c_str()),
        connect_((std::wstring(ConnectLinkSectionName())+scope).c_str(),(std::wstring(ConnectLinkEventName())+scope).c_str()),
        replay_((std::wstring(ReplayLinkSectionName())+scope).c_str(),(std::wstring(ReplayLinkEventName())+scope).c_str()),
        discord_((std::wstring(DiscordLaunchSectionName())+scope).c_str(),(std::wstring(DiscordLaunchEventName())+scope).c_str()) {}
    bool Open() { return join_.Open()&&match_.Open()&&room_.Open()&&connect_.Open()&&replay_.Open()&&discord_.Open(); }
    bool Take(HomeLaunchLinks& links) {
        bool received=false;
        auto join=join_.Take();if(!join.empty()){links.join=std::move(join);received=true;}
        auto match=match_.Take();if(match.Valid()){links.match=std::move(match);received=true;}
        auto room=room_.Take();if(room.Valid()){links.room=std::move(room);received=true;}
        auto connect=connect_.Take();if(!connect.empty()){links.connect=std::move(connect);received=true;}
        auto replay=replay_.Take();if(!replay.empty()){links.replay=std::move(replay);received=true;}
        DiscordLaunchSlot slot;
        if(discord_.Take(slot)&&slot.launch==1){links.discord=true;received=true;}
        return received;
    }
private:
    JoinLinkMailbox join_; MatchLinkMailbox match_; PublicRoomLinkMailbox room_;
    ConnectLinkMailbox connect_; ReplayLinkMailbox replay_;
    SlotMailbox<DiscordLaunchSlot> discord_;
};
} }
