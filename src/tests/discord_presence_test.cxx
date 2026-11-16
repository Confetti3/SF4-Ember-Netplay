#include "../discord/Presence.hxx"
#include "../discord/Ticket.hxx"
#include <iostream>
#include "test_support.hxx"
int main() {
    using namespace sf4e;
    netplay::Snapshot sessionView;
    room::Snapshot roomView;
    discord::PresenceInput in(sessionView, roomView);
    CHECK(discord::Describe(in).activity=="Starting Ember");
    in.ready=true; CHECK(discord::Describe(in).activity=="In menus");
    in.offline=true; CHECK(discord::Describe(in).activity=="Playing offline");
    sessionView.room=netplay::RoomState::Joined; sessionView.control=netplay::Health::Healthy;
    room::Member local; local.id=2; local.host=false;
    roomView.members.push_back(local); roomView.localMember=2; roomView.capacity=16;
    in.party="ember:test"; in.secret=std::string(127,'a'); in.expires=200; in.now=100;
    const auto open=discord::Describe(in);
    CHECK(open.activity=="In a room" && open.size==1 && open.capacity==16 && open.secret==in.secret);
    roomView.members[0].status=room::MemberStatus::Playing;
    CHECK(discord::Describe(in).activity=="Fighting");
    roomView.members[0].status=room::MemberStatus::Watching;
    CHECK(discord::Describe(in).activity=="Spectating");
    roomView.members[0].status=room::MemberStatus::Queued;
    CHECK(discord::Describe(in).activity=="Queued");
    roomView.members[0].status=room::MemberStatus::Ready;
    CHECK(discord::Describe(in).activity=="Ready");
    roomView.locked=true; CHECK(discord::Describe(in).secret.empty()); roomView.locked=false;
    roomView.capacity=1; CHECK(discord::Describe(in).secret.empty()); roomView.capacity=16;
    in.invites=false; CHECK(discord::Describe(in).secret.empty()); in.invites=true;
    // Describe does not know Privacy: a Hidden room is still offerable by invite. Discord's publisher withholds its secret by OpenToFriends.
    roomView.friendsHidden=true;
    CHECK(discord::Describe(in).secret==in.secret && discord::Describe(in).Offerable() && !roomView.OpenToFriends());
    roomView.friendsHidden=false; CHECK(roomView.OpenToFriends());
    // A room whose snapshot is bound to a tournament match is never offered to friends.
    roomView.tournament.matchId="emt_1";
    CHECK(roomView.tournament.Active() && discord::Describe(in).secret.empty() && discord::Describe(in).activity=="Ready");
    roomView.tournament=room::TournamentBinding{}; CHECK(discord::Describe(in).secret==in.secret);
    in.show=false; CHECK(!discord::Describe(in).show && discord::Describe(in).party.empty()); in.show=true;
    in.now=200; CHECK(discord::Describe(in).secret.empty()); in.now=100;
    sessionView.control=netplay::Health::Lost; CHECK(discord::Describe(in).secret.empty());
    roomView.localMember=0; CHECK(discord::Describe(in).party.empty());

    discord::PendingInvite pending;
    CHECK(pending.Offer("ticket","party",200,1,5,false));
    CHECK(!pending.Offer("ticket","party",200,1,5,false));
    CHECK(pending.Tick(100,5,"",false,true,true)==discord::Next::None);
    CHECK(pending.Tick(100,5,"",true,true,false)==discord::Next::None);
    CHECK(pending.Tick(100,5,"",true,true,true)==discord::Next::Join);
    pending.Cancel(); CHECK(!pending.Active());
    pending.Offer("ticket","party",200,2,5,true);
    CHECK(pending.NeedsConfirmation());
    CHECK(pending.Tick(100,5,"other",true,false,false)==discord::Next::None);
    CHECK(!pending.Confirm(4,2)); CHECK(pending.Confirm(5,2));
    CHECK(pending.Tick(100,5,"other",false,false,false)==discord::Next::None);
    CHECK(pending.Tick(100,5,"other",true,false,false)==discord::Next::Leave);
    pending.LeaveQueued();
    CHECK(pending.Tick(100,5,"",true,false,false)==discord::Next::None);
    CHECK(pending.Tick(100,5,"",true,true,true)==discord::Next::Join);
    CHECK(pending.Tick(100,6,"",true,true,true)==discord::Next::None && !pending.Active());
    pending.Offer("ticket","party",200,3,6,false);
    CHECK(pending.Tick(200,6,"",true,true,true)==discord::Next::Expired);
    pending.Offer("ticket","party",300,4,6,false);
    CHECK(pending.Tick(201,6,"party",true,false,false)==discord::Next::None && !pending.Active());
    pending.Offer("old","party",300,5,6,false);
    pending.Offer("new","other",300,6,6,true);
    CHECK(pending.Secret()=="new" && pending.NeedsConfirmation());
    CHECK(!pending.Matches(5) && !pending.Confirm(6,5));
    CHECK(pending.Matches(6) && pending.Confirm(6,6));
    std::string party; std::uint64_t expires=0;
    CHECK(!discord::TicketMetadata("emd1:"+std::string(122,'!'),party,expires));
    CHECK(!discord::TicketMetadata("emd1:"+std::string(121,'A'),party,expires));
    CHECK(!discord::TicketMetadata("emd1:"+std::string(122,'A'),party,expires));
    // The description, typed, for what is built from it besides the companion's text.
    {
        netplay::Snapshot typedSession;
        room::Snapshot typedRoom;
        discord::PresenceInput typed(typedSession, typedRoom);
        CHECK(discord::Describe(typed).where==discord::Presence::Where::Starting);
        typed.ready=true;
        CHECK(discord::Describe(typed).where==discord::Presence::Where::Menus);
        typed.offline=true;
        CHECK(discord::Describe(typed).where==discord::Presence::Where::Offline);
        typedSession.room=netplay::RoomState::Joined; typedSession.control=netplay::Health::Healthy;
        room::Member self; self.id=2; self.status=room::MemberStatus::Ready; self.training=true;
        typedRoom.members.push_back(self); typedRoom.localMember=2; typedRoom.capacity=16;
        auto inRoom=discord::Describe(typed);
        CHECK(inRoom.where==discord::Presence::Where::Room && inRoom.status==room::MemberStatus::Ready && inRoom.training && inRoom.Offerable());
        // Whether the room could be offered does not depend on having an invitation to offer.
        CHECK(inRoom.secret.empty());
        CHECK(inRoom.held==discord::Presence::Held::None);
        typedRoom.locked=true; CHECK(!discord::Describe(typed).Offerable() && discord::Describe(typed).held==discord::Presence::Held::Locked); typedRoom.locked=false;
        typedRoom.capacity=1; CHECK(!discord::Describe(typed).Offerable() && discord::Describe(typed).held==discord::Presence::Held::Full); typedRoom.capacity=16;
        typedSession.control=netplay::Health::Lost; CHECK(!discord::Describe(typed).Offerable() && discord::Describe(typed).held==discord::Presence::Held::Unhealthy); typedSession.control=netplay::Health::Healthy;
        typedRoom.tournament.matchId="emt_1"; CHECK(!discord::Describe(typed).Offerable() && discord::Describe(typed).held==discord::Presence::Held::Match);
    }
    // A friend's invitation has no companion count to be ordered by; its revision follows the last one's and never goes back.
    {
        discord::PendingInvite pending;
        CHECK(pending.Offer("a","party",300,5,6,false) && pending.Revision()==5);
        CHECK(pending.From()==discord::InviteOrigin::Discord);
        CHECK(pending.OfferFromFriend("b","other",300,6,true,discord::InviteOrigin::Friend) && pending.Revision()==6 && pending.NeedsConfirmation());
        CHECK(pending.From()==discord::InviteOrigin::Friend);
        CHECK(!pending.OfferFromFriend("b","other",300,6,true,discord::InviteOrigin::Friend));
        CHECK(!pending.Matches(5) && pending.Matches(6));
        CHECK(pending.Offer("c","party",300,6,6,false) && pending.Revision()==7);
        CHECK(pending.From()==discord::InviteOrigin::Discord);
        CHECK(!pending.Offer("d","party",300,6,6,false));
        CHECK(pending.Offer("d","party",300,7,6,false) && pending.Revision()==8);
        // The player's own Join on a friend's room is its own origin, which a friend's invite is not; neither is told as Discord's.
        CHECK(pending.OfferFromFriend("e","other",300,6,false,discord::InviteOrigin::OwnJoin) && pending.From()==discord::InviteOrigin::OwnJoin);
        CHECK(std::string(discord::ExpiredWords(discord::InviteOrigin::Discord))=="discord.invitation_expired" && std::string(discord::ExpiredWords(discord::InviteOrigin::Friend))=="room.invitation_expired" &&
            std::string(discord::ExpiredWords(discord::InviteOrigin::OwnJoin))=="room.invitation_expired");
        pending.Cancel(); CHECK(pending.From()==discord::InviteOrigin::Discord);
    }
    return 0;
}
