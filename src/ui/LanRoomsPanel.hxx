#pragma once
#include "MenuRows.hxx"
#include "../netplay/LanRooms.hxx"
#include "../common/Localization.hxx"
#include <algorithm>

namespace sf4e { namespace ui {
inline std::vector<MenuEntry> LanRoomRows(const netplay::LanRooms& view,const std::string& build,bool canJoin) {
    std::vector<MenuEntry> rows;
    auto rooms=view.rooms;
    std::sort(rooms.begin(),rooms.end(),[](const auto& a,const auto& b){return a.name==b.name?a.endpoint<b.endpoint:a.name<b.name;});
    for(const auto& room:rooms){
        const bool compatible=room.build==build,full=room.members>=room.capacity;
        rows.push_back(Row("lan-join-"+room.endpoint,room.name,
            !compatible?loc::T("lan.incompatible"):full?loc::T("lan.full"):loc::T("lan.join_detail"),canJoin&&compatible&&!full));
        rows.back().value=std::to_string(room.members)+" / "+std::to_string(room.capacity);
        rows.back().hint=loc::T("online.join");
    }
    if(rows.empty())rows.push_back(InfoRow("lan-empty",view.status=="unavailable"?loc::T("lan.unavailable"):loc::T("lan.searching"),"",loc::T("lan.search_detail")));
    rows.push_back(Row("lan-host",loc::T("lan.host"),loc::T("lan.host_detail"),canJoin));
    return rows;
}
} }
