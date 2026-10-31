// The room's rows, one builder per room screen (RoomEntries): the board's,
// in the order the board draws them, the table's options, the members, one
// member, the chat and the room's settings; and what sends their presses to
// the room (SendRoom). The board is drawn from these rows in
// RoomPanelTables.cxx, RoomPanelColumn.cxx and RoomPanelArcade.cxx.
#include "ApplicationShell.hxx"
#include "Theme.hxx"
#include "MenuRows.hxx"
#include "RoomFeedback.hxx"
#include "RoomControls.hxx"
#include "RoomCards.hxx"
#include "RowId.hxx"
#include "LatencyMark.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cstdio>
#include <utility>

namespace sf4e { namespace ui {
using namespace room_controls;
namespace {
const room::Member* Member(const room::Snapshot& snapshot, room::MemberId id) {
    return room::FindMember(snapshot, id);
}
const char* Name(const room::Snapshot& snapshot, room::MemberId id) {
    const auto* member = Member(snapshot, id);
    return member ? member->name.c_str() : loc::T(id ? "room.member_left" : "room.open_seat");
}
const char* StatusName(room::MemberStatus status) {
    switch (status) {
    case room::MemberStatus::Queued: return loc::T("room.status.queued");
    case room::MemberStatus::Seated: return loc::T("room.status.seated");
    case room::MemberStatus::Ready: return loc::T("room.status.ready");
    case room::MemberStatus::Playing: return loc::T("room.status.playing");
    case room::MemberStatus::Watching: return loc::T("room.status.watching");
    case room::MemberStatus::WatchingNext: return loc::T("room.status.watching_next");
    default: return loc::T("room.status.idle");
    }
}
// "Idle 4 min" for a member who has done nothing in the room for a minute
// or more while waiting (not in a game, not ready, not watching one), else
// empty. A waiting member who said they are in Training is shown as that
// instead: it is why they are quiet. The room stamped the idle time
// `elapsedMs` ago (ShellView::roomElapsedMs).
std::string IdleText(const room::Member& member, std::uint64_t elapsedMs) {
    const bool waiting = member.status == room::MemberStatus::Idle || member.status == room::MemberStatus::Queued ||
        member.status == room::MemberStatus::Seated;
    if (waiting && member.training) return loc::T("room.in_training");
    const auto idleSeconds = RoomIdleSeconds(member, elapsedMs);
    if (!waiting || idleSeconds < 60) return {};
    const unsigned minutes = idleSeconds / 60;
    return minutes < 60 ? loc::Tf("room.idle_minutes", minutes) : loc::Tf("room.idle_hours", minutes / 60, minutes % 60);
}
// A member with no seat and no queue place anywhere gets the chooser from a
// table card. Watching elsewhere is no obstacle: sitting down ends it.
// A room bound to a tournament match seats its fighters itself and refuses
// every seat change, so nobody there is offered the chooser.
bool ChoosesSeat(const room::Snapshot& s, const room::Table& t) {
    if (s.tournament.Active() || !Member(s, s.localMember)) return false;
    const auto place = room::PlaceOf(s, s.localMember);
    return place.kind == room::Place::Kind::None;
}
// The board's member groups, in their order: playing, at a table, watching, in the room.
int MemberGroup(room::MemberStatus status) {
    switch (status) {
    case room::MemberStatus::Playing: return 0;
    case room::MemberStatus::Seated: case room::MemberStatus::Ready: case room::MemberStatus::Queued: return 1;
    case room::MemberStatus::Watching: case room::MemberStatus::WatchingNext: return 2;
    default: return 3;
    }
}
// A member's status line, wherever it shows: on the board's card, with its
// slot and lock-in, or after the name in a member row, with a local mute. A
// watcher's is its tag instead, and HOST goes only with the host's powers.
std::string MemberStatusText(const room::Snapshot& s, const room::Member& m, bool muted, bool board) {
    if (room::IsWatcher(s, m.id)) return loc::Tf(muted ? "room.member_status_muted" : "room.member_status", WatcherTag(s, m));
    const bool host = m.id == room::ActingHost(s);
    if (board)
        return loc::Tf(m.table >= 0 ? (host ? "room.board_status_host_slot" : "room.board_status_slot") : (host ? "room.board_status_host" : "room.board_status"),
            StatusName(m.status), m.table + 1) + (m.spectatorLocked ? loc::T("room.suffix_locked_in") : "");
    return loc::Tf(host ? (muted ? "room.member_status_host_muted" : "room.member_status_host") : (muted ? "room.member_status_muted" : "room.member_status"),
        StatusName(m.status));
}
// The same local pick is editable before and after taking a seat. Select
// opens its page; Left and Right step only the Ultra and color in place.
void SelectionRows(std::vector<MenuEntry>& rows,const ShellView& v) {
 const bool canChange=RoomActionsAvailable(v)&&v.canEditSelection&&!v.room.localTerminalPending;
 rows.push_back(Row("selection",loc::T("selection.fighter"),canChange?
  std::string(loc::T("room.change_fighter.detail"))+"\n"+v.selectionSummary:SelectionBlocker(v),canChange));
 rows.back().value=v.fighterName;
 const auto stepRow=[&](const char* id,const char* label,const std::string& value,const char* detail,bool steps){
  rows.push_back(Value(id,label,value,canChange?detail:SelectionBlocker(v),canChange));
  rows.back().adjustable=canChange&&steps;rows.back().opens=true;
 };
 stepRow("ultra",loc::T("selection.ultra_combo"),v.ultraName,loc::T("selection.ultra_row.detail"),v.ultraSteps);
 stepRow("appearance",loc::T("selection.appearance"),v.appearanceName,loc::T("room.appearance.detail"),v.colorSteps);
 rows.push_back(Value("fighter-options",loc::T("room.fighter_options"),v.fighterOptionsName,
  canChange?std::string(loc::T("selection.additional_options.detail")):SelectionBlocker(v),canChange));
 rows.back().adjustable=false;rows.back().opens=true;
}
// Your own seat's row on the board, a cabinet or a table card: A readies or
// unreadies (when neither can be sent the row still names the state, but has
// no action to offer), and a leave that gives something up asks first, on
// this row (`asked`: Leave seat asked it here).
ReadyControl OwnSeatRow(MenuEntry& row,const ShellView& v,const room::Table& table,int seat,bool asked){
 const auto control=DescribeReady(v,table,seat);
 row.hint=control.label;row.info=control.kind==ReadyControl::None;
 if(const auto cost=room::CostOfLeavingSeat(table);asked&&cost)row.choices={
  {"stay",loc::T("common.cancel"),loc::T("room.leave_seat.keep")},
  {"leave-seat",loc::T("room.leave_seat"),LeavingCostText(cost,table)}};
 return control;
}
// An Arcade room's floor: its sixteen cabinets. Select on a free one sits
// there (or moves there between games), on your own it readies, or after a
// set asks Rematch or New opponent; on anyone else's it opens their table.
// Under the cabinets, who plays whom: a row per pair, its two cabinets and
// its score. A opens that pair's table options, to look or to watch.
void CabinetRows(std::vector<MenuEntry>& rows,const ShellView& v,bool mutableRoom,int leaveAsk){
 using room::Place;
 const auto& s=v.room;
 const auto mine=room::PlaceOf(s,s.localMember);
 const int ownCabinet=room::CabinetOf(s,s.localMember);
 for(std::size_t c=0;c<room::CabinetCount;++c){
  const auto* m=Member(s,s.cabinets[c]);
  rows.push_back(Row("cabinet-"+std::to_string(c),loc::Tf("room.cabinet_row",c+1,m?m->name:std::string(loc::T("room.cabinet_open"))),
   m?CabinetState(s,c):loc::T(ownCabinet>=0?"room.arcade_move_detail":"room.arcade_sit_detail")));
  auto& row=rows.back();row.detailText=DetailText::Name;
  if(!m){
   const bool between=ownCabinet>=0?!room::GameUnderWay(s.tables[mine.table]):mine.kind==Place::Kind::None;
   row.hint=loc::T("room.arcade_sit");row.enabled=between&&mutableRoom;
   continue;
  }
  // What a table card's banner says about this seat leads, as on the cabinet's screen.
  if(m->table>=0)if(const auto banner=DescribeTableBanner(v,s.tables[m->table]);!banner.text.empty()&&(banner.seat<0||banner.seat==m->seat))
   row.detail=banner.text+"\n"+row.detail;
  if(static_cast<int>(c)!=ownCabinet||mine.kind!=Place::Kind::Seat)continue;
  const auto& table=s.tables[mine.table];
  if(table.deciding){
   row.choices={{"rematch",loc::T("room.rematch"),loc::T("room.rematch_detail")},{"new-opponent",loc::T("room.new_opponent"),loc::T("room.new_opponent_detail")}};
   row.choices[0].enabled=!table.rematch[mine.seat];
   continue;
  }
  OwnSeatRow(row,v,table,mine.seat,leaveAsk==mine.table);
 }
 for(std::size_t i=0;i<s.tables.size();++i){
  const auto& table=s.tables[i];
  if(!table.p1||!table.p2)continue;
  rows.push_back(Row("matchup-"+std::to_string(i),loc::Tf("room.matchup",Name(s,table.p1),Name(s,table.p2)),
   loc::Tf("room.matchup_detail",room::CabinetOf(s,table.p1)+1,room::CabinetOf(s,table.p2)+1)));
  rows.back().value=SetScoreText(table.score);rows.back().hint=loc::T("room.table_options");rows.back().userText=true;
 }
}
// A Classic room's floor: a card per table in play. A member with no place
// gets the seat chooser from it; on your own seat A readies; on the table you
// queue at, A opens its options.
void TableCardRows(std::vector<MenuEntry>& rows,const ShellView& v,int leaveAsk){
 using room::Place;
 const auto& s=v.room;
 const auto mine=room::PlaceOf(s,s.localMember);
 const auto* local=Member(s,s.localMember);
 for(std::size_t i=0;i<room::TablesInPlay(s);++i){
  const auto& table=s.tables[i];
  const auto occupied=(table.p1?1:0)+(table.p2?1:0);
  std::string detail=loc::Tf("room.table_detail",Name(s,table.p1),Name(s,table.p2),PhaseName(table),
   table.queue.size(),table.spectators.size()+table.watchingNext.size(),local?StatusName(local->status):loc::T("room.connecting"));
  rows.push_back(Row("table-"+std::to_string(table.id),loc::Tf("room.table_occupancy",table.id+1,occupied),detail));
  auto& row=rows.back();row.detailText=DetailText::Name;row.key=table.id;
  const bool ownTable=mine.table==static_cast<int>(table.id);
  if(ChoosesSeat(s,table))row.choices=SeatChoices(v,table);
  else if(mine.kind==Place::Kind::Seat&&ownTable){
   const auto control=OwnSeatRow(row,v,table,mine.seat,leaveAsk==static_cast<int>(table.id));
   // During a held start the card says what it waits for, and that B,
   // Cancel the start, calls it off.
   if(table.spectatorHold&&control.kind==ReadyControl::Unready)row.detail=control.detail;
  }
  else if(mine.kind==Place::Kind::Queue&&ownTable)row.hint=loc::T("room.table_options");
 }
}
// The Members panel: each member under their group's heading, so the pad
// walks them in the order they are drawn, then the heading, which opens the
// Members list, and the panel's Hide; folded, the heading and its Show. Hide
// and Show change the board's layout, so they are `open` as its settings are
// (ShellView::CanEditRoomSettings), and so are Chat's.
void MembersPanelRows(std::vector<MenuEntry>& rows,const room::Snapshot& s,bool shown,bool open){
 if(shown){
  std::vector<const room::Member*> members;for(const auto& m:s.members)members.push_back(&m);
  std::stable_sort(members.begin(),members.end(),[](const room::Member* a,const room::Member* b){return MemberGroup(a->status)<MemberGroup(b->status);});
  static const char* const groups[]={"room.v2.group_playing","room.v2.group_at_table","room.v2.group_watching","room.v2.group_in_room"};
  for(const auto* m:members){
   rows.push_back(Row("member-"+std::to_string(m->id),loc::Tf(m->id==s.localMember?"room.member_you":"room.member",m->name),MemberStatusText(s,*m,false,false)));
   rows.back().userText=true;rows.back().key=m->id;rows.back().group=loc::T(groups[MemberGroup(m->status)]);
  }
 }
 rows.push_back(Row("room-members",loc::T("room.members"),loc::Tf("room.member_count",s.members.size(),s.capacity)));
 rows.push_back(shown?Row("room-hide-members",loc::T("room.v2.hide_members"),loc::T("room.v2.hide_detail"),open):
  Row("room-show-members",std::string(loc::T("room.members"))+" \xC2\xB7 "+loc::T("room.v2.show"),loc::T("room.v2.show_detail"),open));
}
// The Chat panel: its message box, which opens Chat, and the panel's Hide;
// folded, its heading's Show alone.
void ChatPanelRows(std::vector<MenuEntry>& rows,unsigned unread,bool shown,bool open){
 if(!shown){rows.push_back(Row("room-show-chat",std::string(loc::T("room.chat"))+" \xC2\xB7 "+loc::T("room.v2.show"),loc::T("room.v2.show_detail"),open));return;}
 rows.push_back(Row("room-chat",loc::T("room.chat"),(unread?loc::Tf("room.chat.unread",unread)+" ":std::string())+loc::T("room.chat.detail")));
 if(unread)rows.back().value=std::to_string(unread);
 rows.push_back(Row("room-hide-chat",loc::T("room.v2.hide_chat"),loc::T("room.v2.hide_detail"),open));
}
}
bool ApplicationShell::SendRoom(room::Action action, const ShellView& view, const Submit& submit) {
    // Button disabled state is a rendering affordance; keyboard/controller
    // activation can still reach an item ID. Recheck the room authority
    // boundary at dispatch so a lost or closed control stream never queues a
    // room mutation.
    if (!RoomTakesNow(view, action.kind, action.table) || !view.room.roomEpoch || !view.room.localMember) {
        Refuse(RoomWaitReason(view), [kind = action.kind, table = action.table](const ShellView& now) {
            return !RoomTakesNow(now, kind, table) || !now.room.roomEpoch || !now.room.localMember;
        });
        return false;
    }
    if (action.table >= view.room.tables.size()) return false;
    // An Unwatch of the game this PC watches is that game's leave, which the
    // runtime scopes to the watched game when it runs.
    if (action.kind == room::ActionKind::Unwatch && LeavesWatchedGame(view, action.table)) return LeaveWatchedGame(view, submit);
    const auto mine = room::PlaceOf(view.room, view.room.localMember);
    const auto& table = view.room.tables[action.table];
    if (TerminalFenced(view, action.kind, action.table)) {
        Refuse(TerminalPendingReason(), [kind = action.kind, index = action.table](const ShellView& now) {
            return TerminalFenced(now, kind, index);
        });
        return false;
    }
    const bool localSeated = mine.kind == room::Place::Kind::Seat && mine.table == static_cast<int>(action.table);
    // An Arcade room's Queue names a cabinet, not the table the pair plays at.
    if (((action.kind == room::ActionKind::Queue && !view.room.arcade) || action.kind == room::ActionKind::Watch ||
        action.kind == room::ActionKind::Unwatch) && localSeated) return false;
    if (action.kind == room::ActionKind::AbortMatch && AbandonableAt(view, table) == AbandonableGame::None) return false;
    if (action.kind == room::ActionKind::Unqueue && localSeated) {
        // The same rule B and the Leave seat row read.
        const auto hold = room::LeavingSeatHold(table, mine.seat, GameLive(view), PostMatch(view));
        if (hold != room::SeatHold::None) { Refuse(loc::T(LeaveBlockerKey(hold)), LeaveBlocked); return false; }
    }
    action.protocolVersion = room::ProtocolVersion;
    action.roomEpoch = view.room.roomEpoch;
    action.revision = view.room.revision;
    if (action.table < view.room.tables.size()) {
        action.tableRevision = table.revision;
		const bool generationScoped = action.kind == room::ActionKind::RecordResult ||
			action.kind == room::ActionKind::MatchFinished || action.kind == room::ActionKind::CancelResult ||
			action.kind == room::ActionKind::AbortMatch ||
			(action.kind == room::ActionKind::Unwatch &&
				(table.phase == room::TablePhase::Playing || table.phase == room::TablePhase::Paused));
		if (!action.matchGeneration && generationScoped)
			action.matchGeneration = table.matchGeneration;
    }
    action.actionId = nextActionId_++;
    ShellAction request;
    request.command.kind = netplay::CommandKind::RoomAction;
    request.command.generation = view.session.generation;
    request.roomAction = std::move(action);
    if (!submit(std::move(request))) { error_ = loc::T("error.queue_failed"); return false; }
    error_.clear(); return true;
}


// What every room screen's rows read of the room, worked out once a frame
// by RoomEntries before it hands the screen to its builder.
struct ApplicationShell::RoomScreenRows {
    const ShellView& v;
    const room::Snapshot& s;
    MenuNavigation& nav;
    // actingHost: this member holds the host's powers in the room's eyes;
    // host: and the room can take a host's change now (RoomActionsAvailable).
    bool mutableRoom, actingHost, host, tournamentRoom, boundTable;
    const room::Member* local;
    // The selected table.
    const room::Table& t;
    bool active;
    room::Place place;
    bool seated, elsewhere, queued, watching;
};
std::vector<MenuEntry> ApplicationShell::RoomEntries(const ShellView& v) {
 using namespace room;
 auto& nav=menu_.navigation; const auto& s=v.room;
 if(roomEpoch_!=s.roomEpoch){roomEpoch_=s.roomEpoch;chat_[0]=0;selectedTable_=0;rulesDirty_=false;leaveAsk_=-1;leaveAsked_=false;
  std::snprintf(roomName_,sizeof(roomName_),"%s",s.name.c_str());roomCapacity_=s.capacity;
  if(const auto* local=Member(s,s.localMember))if(local->table>=0&&local->table<static_cast<int>(s.tables.size()))selectedTable_=local->table;
 }
 std::vector<MenuEntry> rows;
  const bool mutableRoom=RoomActionsAvailable(v);
  const bool actingHost=s.localMember&&s.localMember==room::ActingHost(s);
  const bool host=mutableRoom&&actingHost;
  // A room bound to a tournament match seats its two fighters at table 0 by
  // itself and keeps everyone else out; the room refuses seat, watch, rules
  // and kick changes there, so they are not offered.
  const bool tournamentRoom=s.tournament.Active();
  const bool boundTable=tournamentRoom&&selectedTable_==room::TournamentTable;
 const auto* local=Member(s,s.localMember);
 const auto& t=s.tables[selectedTable_];
 const auto screen=nav.Screen();
 // A question about leaving a seat belongs to the board it was asked on.
 if(screen!="room"){leaveAsk_=-1;leaveAsked_=false;}
 // A new game at the same table closes a dialog about the last one; moving to
 // another table (a click opens its chooser a frame before this catches up)
 // does not. Text being typed is not about a game, and only the board and the
 // table's options hold dialogs about one.
 if(generationTable_!=selectedTable_){generationTable_=selectedTable_;tableGeneration_=t.matchGeneration;}
 else if(tableGeneration_!=t.matchGeneration){
  tableGeneration_=t.matchGeneration;
  if(!nav.Editing()&&(screen=="room"||screen=="room-table"))nav.Cancel();
 }
 const bool active=t.phase==TablePhase::Ready||t.phase==TablePhase::Playing||t.phase==TablePhase::Paused;
 const auto place=room::PlaceOf(s,s.localMember);
 const bool seated=place.kind==room::Place::Kind::Seat&&place.table==selectedTable_;
 // A seat or queue place at another table blocks joining this one. Watching
 // elsewhere does not, because Queue and Watch both end it (the authority's
 // StopWatching); the board's seat chooser applies the same rule.
 const bool elsewhere=place.kind!=room::Place::Kind::None&&place.table!=selectedTable_;
 const bool queued=std::find(t.queue.begin(),t.queue.end(),s.localMember)!=t.queue.end();
 // A queued member is listed as a spectator of the game it waits out without
 // having chosen to watch it.
 const bool watching=room::WatchesByChoice(t,s.localMember);
 const RoomScreenRows c{v,s,nav,mutableRoom,actingHost,host,tournamentRoom,boundTable,local,t,active,place,seated,elsewhere,queued,watching};
 if(screen=="room")RoomBoardRows(rows,c);
 else if(screen=="room-table")RoomTableRows(rows,c);
 else if(screen=="room-members")RoomMembersRows(rows,c);
 else if(screen=="room-member")RoomMemberRows(rows,c);
 else if(screen=="room-chat")RoomChatRows(rows,c);
 else if(screen=="room-admin")RoomAdminRows(rows,c);
 // A member's actions open at their row, not on a page of their own. Your own
 // row has none to offer, so it only informs.
 for(auto& row:rows){
  const auto id=RowKey(row.id,"member-");
  if(!id)continue;
  if(*id==s.localMember){row.info=true;continue;}
  const auto* m=Member(s,*id);
  if(!m)continue;
  row.choices={{"mute",loc::T(muted_.count(*id)?"room.unmute_member":"room.mute_member"),loc::T("room.mute_member.detail")},
   {"steam-profile",loc::T("room.steam_profile"),loc::T(m->steamId.Present()?"room.steam_profile.detail":"room.steam_profile.none"),m->steamId.Present()}};
  if(!tournamentRoom)row.choices.push_back({"kick",loc::T("room.kick_member"),loc::Tf("room.kick_member.detail",m->name),host});
  row.choices.push_back({"transfer-host",loc::T("room.transfer_host"),loc::Tf("room.transfer_host.detail",m->name),host&&!room::IsWatcher(s,*id)});
  row.popout=true;
 }
 if(!active) {
  for(auto& row:rows)if(row.id=="selection"||row.id=="ultra"||row.id=="appearance"||row.id=="stage"||row.id=="fighter-options"||row.id=="check-connection"||
   row.id=="input-delay"||row.id=="queue"||row.id=="watch") {
    // Per table: a cached explanation never shows on another table's row.
    const auto key=screen+"/"+std::to_string(selectedTable_)+"/"+row.id;
    if(roomUpdateVisible_)row.detail=RoomWaitReason(v);
    else if(RoomCheckpointPending(v)) {
     // Only reuse an explanation for the same action. Labels, values and
     // eligibility always come from the live snapshot, never this cache.
     const auto previous=roomDetails_.find(key);
     if(row.detail==RoomWaitReason(v)&&previous!=roomDetails_.end())
      row.detail=previous->second;
    }else if(mutableRoom)roomDetails_[key]=row.detail;
   }
 }
 return rows;
}
void ApplicationShell::RoomBoardRows(std::vector<MenuEntry>& rows,const RoomScreenRows& c) {
 using namespace room;
 const ShellView& v=c.v;
 const room::Snapshot& s=c.s;
 MenuNavigation& nav=c.nav;
 // The board's rows in the order it draws them: the floor, the Members
 // panel, the Chat panel, the picture's Hide, then the action bar (Room.dc.html).
 if(s.roomEpoch){
  if(s.arcade)CabinetRows(rows,v,c.mutableRoom,leaveAsk_);else TableCardRows(rows,v,leaveAsk_);
  MembersPanelRows(rows,s,preferences_.roomShowMembers,v.CanEditRoomSettings());
  ChatPanelRows(rows,transcript_.Unread(muted_),preferences_.roomShowChat,v.CanEditRoomSettings());
 }else rows.push_back(Row("room-status",loc::T("room.connection_status"),loc::T(v.session.room==netplay::RoomState::Opening?"room.opening":"room.waiting_state"),false));
 if(pictureOnBoard_)rows.push_back(Row("room-hide-picture",loc::T("room.v2.hide_picture"),loc::T("room.v2.hide_detail"),v.CanEditRoomSettings()));
 // The action bar: Ready when seated, Training, the invitation's two copy
 // buttons side by side, More…, the asking rows, then Leave room last.
 if(s.roomEpoch&&c.place.kind==Place::Kind::Seat){
  const auto ready=DescribeReady(v,s.tables[c.place.table],c.place.seat);
  // A bar button is a few words wide, so Unready drops its "unlock fighter", as on the card's strip.
  rows.push_back(Row("ready",ready.kind==ReadyControl::Unready?loc::T("room.unready.short"):ready.label,ready.refusal.empty()?ready.detail:ready.refusal,ready.kind!=ReadyControl::None));
 }
 if(s.roomEpoch)rows.push_back(Row("room-training",loc::T("room.training"),loc::T(v.canTrain?"room.training.detail":"room.training.unavailable"),v.canTrain));
 // A tournament room admits only its two fighters, who reach it through the
 // tournament service, so there is no invitation to hand out. A public room
 // admits by the service's ticket, so its invitation would admit nobody.
 // The tournament's own room is never shared, from its first claim: the window
 // before its binding lands has no tournament snapshot yet.
 if(!c.tournamentRoom&&!s.serverOwned&&!v.tournament.MatchRoomActive()){
  rows.push_back(Row("copy",loc::T("room.copy_invitation"),loc::T("room.copy_invitation.detail"),!v.invitation.empty()));
  // Once made, the detail shows the link itself so it can be read out.
  rows.push_back(Row("copy-short",loc::T("room.copy_short_invitation"),v.shortInvitation.empty()?std::string(loc::T("room.copy_short_invitation.detail")):
   v.shortInvitation.substr(v.shortInvitation.find("//")==std::string::npos?0:v.shortInvitation.find("//")+2),!v.invitation.empty()));
 }
 // A public room is shared by its page link, which opens it in Ember from a browser or Discord.
 if(s.serverOwned){const auto link=publicRooms_.RoomLink();if(!link.empty())rows.push_back(Row("copy-room-link",loc::T("public.copy_link"),loc::Tf("public.copy_link_detail",link)));}
 // More… (a popout): the table options, the controllers and the room's
 // settings, then the board's parts as switches, which Select flips and More…
 // stays open on; or, chosen from it, the picture's Small, Big and Off in its
 // place, on the one in effect. The switches and the picture's choices are
 // the board's layout, open as its settings are (ShellView::CanEditRoomSettings).
 {
  const bool layoutOpen=v.CanEditRoomSettings();
  MenuEntry more=Row("room-more",loc::T("room.v2.more"),loc::T("room.v2.more_detail"));more.popout=true;
  if(!moreAsk_&&!nav.Choosing()){morePicture_=false;moreChosen_.clear();}
  more.chosen=moreChosen_;
  const char* const pictureNames[]={"size.small","size.large","room.v2.picture_none"};
  if(morePicture_){
   more.label=loc::T("room.v2.picture");more.chosen="picture-"+std::to_string(preferences_.pictureMode);
   const char* const details[]={"room.v2.picture_small_detail","room.v2.picture_big_detail","room.v2.picture_off_detail"};
   for(int i=0;i<3;++i)more.choices.push_back({"picture-"+std::to_string(i),loc::T(pictureNames[i]),loc::T(details[i]),layoutOpen});
  }else{
   if(s.roomEpoch){
    // Changing fighter needs a seat, so it lives in the table options (and X).
    more.choices.push_back({"options",loc::T("room.table_options"),loc::Tf("room.table_options.detail",OptionsTable(v,selectedTable_)+1)});
    // The controller and its buttons, one press from the board.
    more.choices.push_back({"room-controls",loc::T("player.controllers"),loc::T("room.controls_detail")});
    more.choices.push_back({"room-admin",loc::T("room.settings"),loc::T(c.host?"room.settings.detail":"room.settings.host_only"),c.host});
   }
   const auto shownPart=[&](const char* part,bool on){return loc::Tf("room.v2.part_state",loc::T(part),loc::T(on?"common.on":"common.off"));};
   more.choices.push_back({"room-show-members",shownPart("room.members",preferences_.roomShowMembers),loc::T("room.v2.part_detail"),layoutOpen});
   more.choices.push_back({"room-show-chat",shownPart("room.chat",preferences_.roomShowChat),loc::T("room.v2.part_detail"),layoutOpen});
   more.choices.push_back({"room-picture",loc::Tf("room.v2.part_state",loc::T("room.v2.picture"),loc::T(pictureNames[preferences_.pictureMode])),loc::T("room.v2.picture_detail"),layoutOpen});
  }
  rows.push_back(std::move(more));
 }
 if(s.roomEpoch){
  // Over a Practice match it fights in, a fighter can end the game.
  if(c.place.kind==Place::Kind::Seat&&AbandonableAt(v,s.tables[c.place.table])==AbandonableGame::Practice)
   rows.push_back(ConfirmRow("end-practice",loc::T("room.end_practice"),loc::T("room.end_practice.detail"),c.mutableRoom));
  // Over a game it only watches, a spectator can leave it; once the room has
  // finished that game, the rest of its stream, which asks nothing of the room.
  if(const auto leave=netplay::LeaveOfWatchedGame(s,v.watchedGame);leave!=netplay::WatchedLeave::None){
   const bool leaveGame=leave==netplay::WatchedLeave::LeaveGame,viewOnly=leave==netplay::WatchedLeave::EndView;
   rows.push_back(ConfirmRow("stop-watching",loc::T(leaveGame?"room.leave_game":"room.stop_watching"),
    loc::T(viewOnly?"room.stop_watching.finished":leaveGame?"room.leave_game.detail":"room.stop_watching.detail"),
    viewOnly||RoomTakesNow(v,ActionKind::Unwatch,static_cast<std::size_t>(v.watchedGame.table))));
  }
 }
 // A public room has no replacement: when its host is gone the room closes.
 if(v.session.recovery==netplay::Recovery::ReplacementOffered&&!s.serverOwned)
  rows.push_back(ConfirmRow("replace-room",loc::T("room.replace"),loc::T(v.canReplaceRoom?"room.replace.detail":"room.replace.waiting"),v.canReplaceRoom));
 rows.push_back(ConfirmRow("leave",loc::T(v.session.room==netplay::RoomState::Closing?"room.leaving":"room.leave"),
  LeaveRoomDetail(v),v.session.room!=netplay::RoomState::Closing));
 // The questions the board asks of its own rows, once they are all there.
 if(s.roomEpoch&&leaveAsk_>=0){
  const auto asked=s.arcade?"cabinet-"+std::to_string(room::CabinetOf(s,s.localMember)):"table-"+std::to_string(leaveAsk_);
  if(!leaveAsked_){if(nav.Ask(asked,rows))leaveAsked_=true;else leaveAsk_=-1;}
  else if(!nav.Choosing()){leaveAsk_=-1;leaveAsked_=false;}
 }
 if(moreAsk_){moreAsk_=false;nav.Ask("room-more",rows);}
}
void ApplicationShell::RoomTableRows(std::vector<MenuEntry>& rows,const RoomScreenRows& c) {
 using namespace room;
 const ShellView& v=c.v;
 const room::Snapshot& s=c.s;
 const bool mutableRoom=c.mutableRoom;
 const bool host=c.host;
 const bool boundTable=c.boundTable;
 const room::Member* const local=c.local;
 const room::Table& t=c.t;
 const bool active=c.active;
 const room::Place& place=c.place;
 const bool seated=c.seated;
 const bool elsewhere=c.elsewhere;
 const bool queued=c.queued;
 const bool watching=c.watching;
   const std::string reason=!mutableRoom?RoomWaitReason(v):
    loc::T(elsewhere?"room.leave_current_table":"room.choose_action");
  // The table's rules: the host edits them in place and applies them in one
  // press (applying clears Ready), everyone else reads them on one line.
  // Which rows these are follows who is host, not the room's health: while the
  // room cannot take a change they stay, disabled, so nothing moves under them.
  const auto rulesRows=[&]{
   if(!rulesDirty_&&rulesRevision_!=t.revision){tableRules_=t.rules;rulesRevision_=t.revision;}
   // An Arcade room's rules are the room's, set when it was created.
   if(!c.actingHost||boundTable||s.arcade){
    std::string summary=loc::Tf(s.practice?"room.rules_summary_practice":"room.rules_summary",static_cast<int>(t.rules.roundCount),static_cast<int>(room::MatchRoundTime(s,t)),
     loc::T(t.rules.editionSelect?"common.on":"common.off"));
    if(s.delayPolicy==room::DelayPolicy::Own)summary=loc::Tf("room.rules_summary_own_delay",summary);
    rows.push_back(ReadOnlyValue("rules",loc::T("room.table_rules"),summary,loc::T(boundTable?"room.rules.tournament":s.arcade?"room.rules.arcade":"room.rules.host_only")));
    rows.push_back(ReadOnlyValue("set-rules",loc::T("room.table_set"),s.arcade?SetLengthText(t.rules.format):SetSummaryText(t.rules),
     t.rules.format==room::SetFormat::Unlimited||s.arcade?loc::T("rules.set_length.detail"):RotationDetail(t.rules.rotation)));
    return;
   }
   RuleRows(rows,tableRules_,host&&!active,!host?reason.c_str():loc::T(active?"room.rules.finish_game":"room.rules.apply_note"),s.practice);
   if(rulesDirty_)rows.push_back(Row("apply-rules",loc::T("room.apply_rules"),loc::T("room.apply_rules.detail"),host&&!active));
   // A name changes nothing at the table, so it is taken during a game too.
   rows.push_back(TextRow("table-name",loc::T("room.table_name"),t.name,room::MaximumTableNameBytes,host));
   rows.back().detail=std::string(loc::T("room.table_name.detail"))+"\n"+loc::T("menu.edit_detail");rows.back().userText=true;
   // The host edits the rules in place, so the summary above is not shown to them.
   if(s.delayPolicy==room::DelayPolicy::Own)rows.push_back(ReadOnlyValue("delay-policy",loc::T("room.delay_policy"),loc::T("room.delay_policy.own"),loc::T("room.delay_policy.detail")));
  };
  if(seated){
   const bool ready=room::ReadyCancellable(t,place.seat);
    const auto readyControl=DescribeReady(v,t,place.seat);
    // A Training call's window or the room's Ready timeout: the row says how
    // long there is to ready.
    const auto deadline=DescribeReadyDeadline(v,t);
    const auto timeout=deadline.seat==place.seat?deadline.text:std::string();
    rows.push_back(Row("ready",readyControl.label,timeout.empty()?readyControl.detail:timeout+"\n"+readyControl.detail,readyControl.kind!=ReadyControl::None));
    if(t.spectatorHold||!timeout.empty())rows.back().detailText=DetailText::Name;
   // Under Ready, in the order a player reads them: their own pick (fighter,
   // Ultra, appearance, fighter options), then the match (P1's stage and the
   // table's rules), then their connection, then leaving.
   SelectionRows(rows,v);
   const bool canChange=mutableRoom&&v.canEditSelection&&!s.localTerminalPending;
   const bool stageOwner=v.localSlot==0;
   rows.push_back(Value("stage",loc::T("selection.stage"),v.stageName,
    !canChange?SelectionBlocker(v):loc::T(stageOwner?"selection.p1_stage":"selection.only_p1_stage"),canChange&&stageOwner));
   rows.back().adjustable=false;rows.back().opens=true;
   rulesRows();
    // One delay row: Left and Right choose it, Select takes the recommendation.
    // Auto sits before the smallest delay. It shows a number once the check
    // has measured this opponent, or once Ready has locked one.
    const bool autoDelay=v.preferences.autoInputDelay;
    const bool delayEditable=mutableRoom&&!active&&!v.delayLocked;
    const int selectedDelay=(std::max)(MinimumInputDelay,(std::min)(MaximumInputDelay,v.selectedDelay));
    const bool recommended=v.recommendedDelay>=0&&v.recommendedDelay<=MaximumInputDelay;
    const auto check=DescribeConnectionCheck(v);
    const bool opponentReady=v.opponentDelay>=0&&v.opponentDelay<=InputDelayWireCeiling;
    std::string delayDetail=loc::Tf("room.input_delay.recommended",check.value);
    // An Own room plays each fighter at their own delay, so there is no match value to show.
    if(t.p1&&t.p2)delayDetail+="\n"+(s.delayPolicy==room::DelayPolicy::Own?loc::Tf("room.input_delay.own",FramesText(selectedDelay)):loc::Tf("room.input_delay.match",opponentReady?
     FramesText(room::MatchDelay(selectedDelay,v.opponentDelay)):loc::Tf(selectedDelay==1?"room.match_delay.at_least_one":"room.match_delay.at_least",selectedDelay)));
    if(!delayEditable)delayDetail+="\n"+(v.delayLocked?std::string(loc::T("room.selected_delay.locked")):reason);
    else{
     delayDetail+="\n"+(autoDelay?AutoDelayDetail(v.preferences.autoDelayStyle):loc::Tf("room.selected_delay.detail",MinimumInputDelay,MaximumInputDelay));
     if(!recommended&&!autoDelay)delayDetail+="\n"+std::string(loc::T("room.apply_recommendation.check_first"));
    }
    const std::string delayFrames=FramesText(selectedDelay);
    const std::string delayValue=!autoDelay?delayFrames:
     v.autoDelayMeasured||v.delayLocked?loc::Tf("room.input_delay.auto",delayFrames):std::string(loc::T("settings.input_delay.auto"));
    rows.push_back(Value("input-delay",loc::T("room.input_delay"),delayValue,delayDetail,delayEditable));
    rows.back().opens=!autoDelay&&v.canApplyDelay&&recommended&&!check.checking;
    if(rows.back().opens)rows.back().hint=loc::T("room.apply_recommendation");
    rows.push_back(Row("check-connection",check.action,check.checking?check.detail:
     !mutableRoom?reason:!t.p1||!t.p2?loc::T("room.check_connection.two_players"):
     ready?loc::T("room.check_connection.unready"):
     !delayEditable?loc::T("room.check_connection.finish_match"):check.detail,
     v.canProbe&&delayEditable&&!check.checking));
    if(check.namesPlayer)rows.back().detailText=DetailText::Name;
    // The same departure rule as B on the board. Leaving that gives up a score
    // asks first, and says what it gives up.
    // During a held start B takes Ready back; this row only ever leaves, so it
    // waits for Ready to be taken back first.
    const auto leave=ExitFromPlace(v);
    const bool leaves=leave.allowed&&!leave.unready;
    std::string leaveDetail=leaves?loc::T(t.queue.empty()?"room.leave_seat.release":"room.leave_seat.next_player"):
     leave.unready?loc::T("room.leave_seat.unready_first"):leave.blocker;
    if(leaves&&leave.cost.score)leaveDetail+=" "+loc::Tf("room.leave_seat.costs_score",SetScoreText(t.score));
    if(!boundTable){
     rows.push_back(Row("unqueue",loc::T("room.leave_seat"),leaveDetail,mutableRoom&&leaves));
     rows.back().confirm=leaves&&bool(leave.cost);
    }
    // The authority accepts AbortMatch from either fighter. Offer it only once
    // the result is Paused, so a fighter can never cut a live game short.
    if(AbandonableAt(v,t)==AbandonableGame::Unresolved)rows.push_back(ConfirmRow("abandon-result",loc::T("room.abandon_result"),
     loc::T("room.abandon_result.detail"),mutableRoom));
   }else{
    SelectionRows(rows,v);
    // Enablement asks the same fence SendRoom dispatches through, so the row
    // and the dispatch guard cannot drift apart.
    const auto tableRow=[&](room::ActionKind kind,const char* id,const char* label){
     const bool fenced=TerminalFenced(v,kind,selectedTable_);
     rows.push_back(Row(id,loc::T(label),(fenced?TerminalPendingReason():reason)+WatchingNote(v,kind),mutableRoom&&!elsewhere&&!fenced));
    };
    if(!boundTable)tableRow(queued?room::ActionKind::Unqueue:room::ActionKind::Queue,queued?"unqueue":"queue",
     queued?"room.leave_queue":s.arcade?"room.arcade_sit":"room.join_queue");
    // A queued member has no watch choice: Watch is refused them, and the game
    // they wait out is theirs to watch until they are seated or leave the queue.
    if(!queued&&!boundTable)tableRow(watching?room::ActionKind::Unwatch:room::ActionKind::Watch,watching?"unwatch":"watch",
     watching?"room.stop_watching":"room.watch_next");
    // A watcher's lock-in is never fenced: the one still leaving the last
    // game is exactly who it is for.
    if(room::WatchesByChoice(t,s.localMember)){
     const bool lockedIn=local&&local->spectatorLocked;
     rows.push_back(Row("lock-spectating",loc::T(lockedIn?"room.unlock_spectating":"room.lock_spectating"),
      mutableRoom?loc::Tf(lockedIn?"room.unlock_spectating.detail":"room.lock_spectating.detail",room::SpectatorStartHoldMs/1000):reason,mutableRoom));
    }
    rulesRows();
  }
  if(t.phase==TablePhase::Paused)rows.push_back(ConfirmRow("cancel-result",loc::T("room.cancel_unresolved"),loc::T("room.cancel_unresolved.detail"),host));
  // A table can be left in Playing when neither fighter's finish report
  // reached the room (both clients lost control at battle close). The
  // authority accepts a host cancel in that phase; offer it to a host who is
  // not one of the fighters, so a live game can never be cut short by its
  // own participant from this row. Only a game that has run without a result
  // for StaleGameSeconds is offered; before that the row says the game is live.
  else if(t.phase==TablePhase::Playing&&host&&!seated&&!t.resultPending){
   const bool stale=GameIsStale(selectedTable_);
   rows.push_back(ConfirmRow("cancel-result",loc::T("room.cancel_stuck"),stale?std::string(loc::T("room.cancel_stuck.detail")):
    loc::Tf("room.cancel_stuck.in_progress",room::StaleGameSeconds/60),stale));
  }
}
void ApplicationShell::RoomMembersRows(std::vector<MenuEntry>& rows,const RoomScreenRows& c) {
 using namespace room;
 const room::Snapshot& s=c.s;
  for(const auto& m:s.members){
   rows.push_back(Row("member-"+std::to_string(m.id),loc::Tf(m.id==s.localMember?"room.member_you":"room.member",m.name),MemberStatusText(s,m,muted_.count(m.id)>0,false)));
   rows.back().value=NetworkLinkName(m.link);rows.back().userText=true;rows.back().key=m.id;
  }
}
void ApplicationShell::RoomMemberRows(std::vector<MenuEntry>& rows,const RoomScreenRows& c) {
 using namespace room;
 const room::Snapshot& s=c.s;
 const bool host=c.host;
 const bool tournamentRoom=c.tournamentRoom;
   const auto* m=Member(s,selectedMember_);const bool other=m&&m->id!=s.localMember;
   rows.push_back(Row("mute",loc::T(muted_.count(selectedMember_)?"room.unmute_member":"room.mute_member"),loc::T(m?"room.mute_member.detail":"room.member_left.detail"),other));
   // Only a member who chose to show the room their profile has one to open.
   const bool hasSteam=m&&m->steamId.Present();
   rows.push_back(Row("steam-profile",loc::T("room.steam_profile"),
    loc::T(!m?"room.member_left.detail":hasSteam?"room.steam_profile.detail":"room.steam_profile.none"),hasSteam));
   if(!tournamentRoom){
    rows.push_back(ConfirmRow("kick",loc::T("room.kick_member"),m?loc::Tf("room.kick_member.detail",m->name):loc::T("room.member_left.detail"),host&&other));
    rows.back().detailText=DetailText::Name;
   }
   rows.push_back(ConfirmRow("transfer-host",loc::T("room.transfer_host"),m?loc::Tf("room.transfer_host.detail",m->name):loc::T("room.member_left.detail"),
    host&&other&&!room::IsWatcher(s,selectedMember_)));
   rows.back().detailText=DetailText::Name;
}
void ApplicationShell::RoomChatRows(std::vector<MenuEntry>& rows,const RoomScreenRows& c) {
 using namespace room;
 const ShellView& v=c.v;
  // The one row: the message box is drawn by DrawChatScreen, and Enter in it is this row's Select. It
  // sends what the box holds, and not again while that same text is on its way. An empty box is a text
  // row instead, so Select opens the keyboard to write in it, the pad's on-screen one included, and its
  // Done sends (RoomAction).
  const bool draft=HasChatText(chat_);
  rows.push_back(Row("compose",loc::T("room.compose_message"),loc::T("chat.detail"),
   ChatTypable(v)&&(!draft||(!queuedChat_&&!(ChatInFlight(ImGui::GetTime())&&pendingChat_->text==chat_)))));
  rows.back().value=chat_;
  if(draft)rows.back().hint=loc::T("chat.send");else{rows.back().text=true;rows.back().textLimit=room::MaximumChatBytes;}
}
void ApplicationShell::RoomAdminRows(std::vector<MenuEntry>& rows,const RoomScreenRows& c) {
 using namespace room;
 const room::Snapshot& s=c.s;
 const bool host=c.host;
  rows.push_back(TextRow("rename",loc::T("room.name"),roomName_,64,host));
  rows.push_back(Value("room-capacity",loc::T("room.capacity"),std::to_string(roomCapacity_),loc::T("room.capacity.detail"),host));
  rows.push_back(Value("lock",loc::T("room.admission"),loc::T(s.locked?"room.locked":"room.open"),loc::T("room.admission.detail"),host));
  rows.push_back(Row("apply-name",loc::T("room.apply_name"),loc::T("room.apply_name.detail"),host&&roomName_[0]));
  rows.push_back(Row("apply-capacity",loc::T("room.apply_capacity"),loc::T("room.apply_capacity.detail"),host&&roomCapacity_>=static_cast<int>(s.members.size())));
  // Kept on this PC for Create › Template; anyone may keep the room they are in.
  rows.push_back(TextRow("save-template",loc::T("room.save_template"),"",32));
  rows.back().detail=std::string(loc::T("room.save_template.detail"))+"\n"+loc::T("menu.edit_detail");
}
MemberCardView ApplicationShell::RoomMemberCard(const ShellView& v,const MenuEntry& e) const {
 MemberCardView card;
 const auto id=RowKey(e.id,"member-");
 const auto* m=id?Member(v.room,*id):nullptr;if(!m)return card;
 card.id=e.id;card.name=m->name;card.label=e.label;card.link=m->link;card.marks={m->country,m->badge};
 card.latency=LatencyMarkFor(v.memberLatency,*m,v.room.localMember);
 card.mainFighter=m->id==v.room.localMember?v.preferences.mainFighter:m->mainFighter;
 const auto status=MemberStatusText(v.room,*m,false,true);
 const auto idle=IdleText(*m,v.roomElapsedMs);
 card.status=idle.empty()?status:status+" \xC2\xB7 "+idle;
 return card;
}
const char* ApplicationShell::PlaceExitLabel(const ShellView& v) const {
 const auto place=ExitFromPlace(v);
 return OnOwnPlace(place,menu_.navigation.Focus())?place.label:"";
}
void ApplicationShell::OpenTableOptions(const ShellView& v,int table) {
 auto& nav=menu_.navigation;
 selectedTable_=table;rulesDirty_=false;rulesRevision_=v.room.tables[table].revision;tableRules_=v.room.tables[table].rules;
 nav.Home();nav.Push("room");nav.Push("room-table");
}
void ApplicationShell::ToggleReady(const ShellView& v,const Submit& submit) {
 const auto* local=Member(v.room,v.room.localMember);
 const auto control=DescribeReady(v,v.room.tables[selectedTable_],local?local->seat:-1);
 if(control.kind==ReadyControl::Unready){
  room::Action request;request.table=static_cast<std::uint8_t>(selectedTable_);
  request.kind=room::ActionKind::Unready;request.seat=local->seat;
  if(!SendRoom(std::move(request),v,submit)&&error_.empty())error_=loc::T("room.action_rejected");
 }else if(control.kind!=ReadyControl::None)
  Send(control.kind==ReadyControl::Rematch?netplay::CommandKind::Rematch:netplay::CommandKind::Ready,v,submit);
 else if(!control.refusal.empty())
  Refuse(control.refusal,[table=selectedTable_,seat=local?local->seat:-1](const ShellView& now){
   return DescribeReady(now,now.room.tables[table],seat).kind==ReadyControl::None;});
}
void ApplicationShell::LeavePlace(const ShellView& v,const Submit& submit) {
 const auto exit=ExitFromPlace(v);if(exit.table<0)return;
 selectedTable_=exit.table;
 if(!exit.allowed){Refuse(exit.blocker,LeaveBlocked);return;}
 room::Action request;request.table=static_cast<std::uint8_t>(selectedTable_);request.kind=room::ActionKind::Unqueue;
 if(exit.unready){request.kind=room::ActionKind::Unready;request.seat=exit.seatIndex;}
 if(!SendRoom(std::move(request),v,submit)&&error_.empty())error_=loc::T("room.action_rejected");
}
bool ApplicationShell::LeaveWatchedGame(const ShellView& v,const Submit& submit) {
 const auto leave=netplay::LeaveOfWatchedGame(v.room,v.watchedGame);if(leave==netplay::WatchedLeave::None)return false;
 const auto table=static_cast<std::size_t>(v.watchedGame.table);selectedTable_=v.watchedGame.table;
 // A leave the room still takes waits for it as any press does; the rest of a
 // finished game's stream asks nothing of the room.
 if(leave!=netplay::WatchedLeave::EndView&&!RoomTakesNow(v,room::ActionKind::Unwatch,table)){
  Refuse(RoomWaitReason(v),[table](const ShellView& now){return LeavesWatchedGame(now,table)&&!RoomTakesNow(now,room::ActionKind::Unwatch,table);});
  return false;
 }
 ShellAction request;request.command.generation=v.session.generation;request.leaveWatchedGame=v.watchedGame;
 if(!submit(std::move(request))){error_=loc::T("error.queue_failed");return false;}
 error_.clear();
 return true;
}
void ApplicationShell::TrackLiveGames(const ShellView& v,double now) {
 for(std::size_t i=0;i<liveGames_.size();++i){
  auto& game=liveGames_[i];const auto& table=v.room.tables[i];
  if(table.phase!=room::TablePhase::Playing)game.since=-1;
  else if(game.since<0||game.since>now||game.generation!=table.matchGeneration){game.generation=table.matchGeneration;game.since=now;}
 }
}
// Leaving the table the lock-in held says why whoever caused it. A lock-in that
// ends while still watching that table is the player's own Release, or the
// release a failed view owes, which the runtime counts instead.
void ApplicationShell::TrackLockIn(const ShellView& v,double now) {
 const auto* local=Member(v.room,v.room.localMember);
 int lockedAt=-1;
 if(local&&local->spectatorLocked)
  for(const auto& t:v.room.tables)if(room::WatchesByChoice(t,local->id)){lockedAt=t.id;break;}
 const char* reason=nullptr;
 if(lockedInTable_>=0&&lockedAt<0&&local&&v.room.roomEpoch==lockInEpoch_&&!room::WatchesByChoice(v.room.tables[lockedInTable_],local->id)){
  bool elsewhere=false;
  for(const auto& t:v.room.tables)elsewhere=elsewhere||room::WatchesByChoice(t,local->id);
  reason=room::PlaceOf(v.room,local->id).kind!=room::Place::Kind::None?"room.lock_in_ended.playing":
   elsewhere?"room.lock_in_ended.moved":"room.lock_in_ended.stopped";
 }
 if(v.lockInDrops>lockInDropsSeen_)reason="room.lock_in_ended.dropped";
 if(reason){ShowNotice(loc::T(reason),Tone::Pending,now+8);}
 lockedInTable_=lockedAt;lockInEpoch_=v.room.roomEpoch;lockInDropsSeen_=v.lockInDrops;
}
bool ApplicationShell::GameIsStale(std::size_t table) const {
 return table<liveGames_.size()&&liveGames_[table].since>=0&&ImGui::GetTime()-liveGames_[table].since>=room::StaleGameSeconds;
}
} }
