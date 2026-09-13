#include "../discord/Presence.hxx"
#include "../discord/Ticket.hxx"
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; return 1; } } while(0)
int main() {
    using namespace sf4e;
    discord::PresenceInput in;
    CHECK(discord::Describe(in).activity=="Starting Ember");
    in.ready=true; CHECK(discord::Describe(in).activity=="In menus");
    in.offline=true; CHECK(discord::Describe(in).activity=="Playing offline");
    in.session.room=netplay::RoomState::Joined; in.session.control=netplay::Health::Healthy;
    room::Member local; local.id=2; local.host=false;
    in.room.members.push_back(local); in.room.localMember=2; in.room.capacity=16;
    in.party="ember:test"; in.secret=std::string(127,'a'); in.expires=200; in.now=100;
    const auto open=discord::Describe(in);
    CHECK(open.activity=="In a room" && open.size==1 && open.capacity==16 && open.secret==in.secret);
    in.room.members[0].status=room::MemberStatus::Playing;
    CHECK(discord::Describe(in).activity=="Fighting");
    in.room.members[0].status=room::MemberStatus::Watching;
    CHECK(discord::Describe(in).activity=="Spectating");
    in.room.members[0].status=room::MemberStatus::Queued;
    CHECK(discord::Describe(in).activity=="Queued");
    in.room.members[0].status=room::MemberStatus::Ready;
    CHECK(discord::Describe(in).activity=="Ready");
    in.room.locked=true; CHECK(discord::Describe(in).secret.empty()); in.room.locked=false;
    in.room.capacity=1; CHECK(discord::Describe(in).secret.empty()); in.room.capacity=16;
    in.invites=false; CHECK(discord::Describe(in).secret.empty()); in.invites=true;
    in.show=false; CHECK(!discord::Describe(in).show && discord::Describe(in).party.empty()); in.show=true;
    in.now=200; CHECK(discord::Describe(in).secret.empty()); in.now=100;
    in.session.control=netplay::Health::Lost; CHECK(discord::Describe(in).secret.empty());
    in.room.localMember=0; CHECK(discord::Describe(in).party.empty());

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
    return 0;
}
