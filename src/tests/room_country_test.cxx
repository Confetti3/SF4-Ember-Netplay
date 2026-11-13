#include "room_authority_support.hxx"
#include "../session/sf4e__SessionProtocol.hxx"

// A member's country through the room: join, snapshot, checkpoint, the member's
// wire form and the join request's.

static void TestMemberCountry() {
    const auto japan=sf4e::CountryCode::Parsed("JP");EXPECT(japan.Present());
    RoomAuthority authority("Flags",16,72);
    MemberProfile shows;shows.country=japan;
    EXPECT(authority.Join("Shows",Peer(0),true,shows).accepted);
    EXPECT(authority.Join("Hides",Peer(1)).accepted);
    const auto snapshot=authority.SnapshotView();
    EXPECT(snapshot.members[0].country==japan&&!snapshot.members[1].country.Present());
    // The member carries its code; one with none has no key at all.
    const auto first=nlohmann::json(snapshot.members[0]),second=nlohmann::json(snapshot.members[1]);
    EXPECT(first.at("country").is_string()&&first.at("country").get<std::string>()=="JP"&&!second.contains("country"));
    // It survives a snapshot and a checkpoint.
    EXPECT(nlohmann::json::parse(nlohmann::json(snapshot).dump()).get<Snapshot>().members[0].country==japan);
    RoomAuthority restored("Restore");EXPECT(restored.RestoreCheckpoint(authority.Checkpoint()));
    EXPECT(restored.SnapshotView().members[0].country==japan&&!restored.SnapshotView().members[1].country.Present());
    // An older peer sends no key; a code the table lacks, or no text, reads as none rather than refusing the member.
    auto member=first;member.erase("country");EXPECT(!member.get<Member>().country.Present());
    for(const nlohmann::json invalid:{nlohmann::json("jp"),nlohmann::json("XK"),nlohmann::json(""),nlohmann::json("JPN"),
        nlohmann::json(81),nlohmann::json(true),nlohmann::json(nullptr)}){
        member["country"]=invalid;EXPECT(!member.get<Member>().country.Present());
    }
}

static void TestJoinRequestCountry() {
    namespace protocol=sf4e::SessionProtocol;
    // A player who shows a country sends its code, and the profile reads it back.
    protocol::SessionJoinRequest request;
    MemberProfile profile;profile.country=sf4e::CountryCode::Parsed("BR");
    request.SetProfile(profile);
    const nlohmann::json sent=request;
    EXPECT(sent.at("country").get<std::string>()=="BR");
    EXPECT(sent.get<protocol::SessionJoinRequest>().Profile().country.ToString()=="BR");
    // None (Hidden, or Windows naming none) sends no key.
    protocol::SessionJoinRequest hidden;hidden.SetProfile(MemberProfile{});
    EXPECT(!nlohmann::json(hidden).contains("country"));
    // A forged value joins as nobody showing a country.
    for(const nlohmann::json invalid:{nlohmann::json("br"),nlohmann::json("ZZ"),nlohmann::json(76),nlohmann::json::array()}){
        auto forged=sent;forged["country"]=invalid;
        EXPECT(!forged.get<protocol::SessionJoinRequest>().Profile().country.Present());
    }
}

int main() {
    TestMemberCountry();
    TestJoinRequestCountry();
    if(TEST_FAILURES())return 1;
    std::puts("RoomCountry test passed");
    return 0;
}
