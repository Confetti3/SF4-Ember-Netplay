#include "room_authority_support.hxx"

// A member's Steam profile through the room: join, snapshot, checkpoint and the wire form.

static void TestProfileSteam() {
    sf4e::SteamId shown;EXPECT(sf4e::SteamId::Parse("76561200000000000",shown));
    RoomAuthority authority("Steam",16,71);
    MemberProfile sharing;sharing.steamId=shown;
    EXPECT(authority.Join("Shows",Peer(0),true,sharing).accepted);
    EXPECT(authority.Join("Hides",Peer(1)).accepted);
    const auto snapshot=authority.SnapshotView();
    EXPECT(snapshot.members[0].steamId==shown&&!snapshot.members[1].steamId.Present());
    // The member carries the exact decimal text; a member who showed nothing has no key at all.
    const auto first=nlohmann::json(snapshot.members[0]),second=nlohmann::json(snapshot.members[1]);
    EXPECT(first.at("steam_id").is_string()&&first.at("steam_id").get<std::string>()=="76561200000000000"&&!second.contains("steam_id"));
    // It survives a snapshot and a checkpoint.
    EXPECT(nlohmann::json::parse(nlohmann::json(snapshot).dump()).get<Snapshot>().members[0].steamId==shown);
    RoomAuthority restored("Restore");EXPECT(restored.RestoreCheckpoint(authority.Checkpoint()));
    EXPECT(restored.SnapshotView().members[0].steamId==shown&&!restored.SnapshotView().members[1].steamId.Present());
    // An older peer sends no key; text that is no SteamId, or not text, reads as none rather than refusing the member.
    auto member=first;member.erase("steam_id");EXPECT(!member.get<Member>().steamId.Present());
    for(const nlohmann::json invalid:{nlohmann::json("76561190000000000"),nlohmann::json("0"),nlohmann::json(""),nlohmann::json("076561200000000000"),
        nlohmann::json(76561200000000000ull),nlohmann::json(true),nlohmann::json("99999999999999999999")}){
        member["steam_id"]=invalid;EXPECT(!member.get<Member>().steamId.Present());
    }
}
int main() {
    TestProfileSteam();
    if (TEST_FAILURES()) return TEST_EXIT_CODE();
    std::puts("RoomSteamProfile test passed");
    return 0;
}
