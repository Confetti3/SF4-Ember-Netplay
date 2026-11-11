#include "UiLabSocial.hxx"
#include <memory>

namespace uilab {
namespace {
using namespace sf4e;
namespace nf = netplay::friends;
constexpr std::uint64_t Now = 1800000000, Hour = 3600, Day = 24 * Hour;

nf::Friend Pal(const char* id, const char* name, const char* country, int fighter, std::uint64_t points, std::uint8_t tier, int badge, std::uint64_t won, std::uint64_t lost) {
    nf::Friend entry;
    entry.id = std::string("fr_") + id; entry.name = name; entry.country = CountryCode::Parsed(country);
    entry.since = Now - 20 * Day;
    entry.hasMain = true; entry.main.fighter = static_cast<std::uint8_t>(fighter);
    entry.main.showsPoints = points > 0; entry.main.points = points; entry.main.tier = tier;
    // The account's points, which the bridge sends beside the main's: a friend with no points in the lab has no card.
    entry.hasAccount = points > 0; entry.account = {points + 120, tier};
    if (badge) entry.badge = SkillBadge::OfClass(static_cast<std::uint8_t>(badge), true);
    entry.hasRecord = won + lost > 0; entry.record = {won, lost, Now - 2 * Day};
    return entry;
}
void InRoom(nf::Friend& entry, nf::Status status, int table, const char* room, bool joinable) {
    auto& p = entry.presence;
    p.kind = nf::Presence::Kind::InRoom; p.status = status; p.table = table; p.hasRoom = true;
    p.room.id = std::string(32, 'a'); p.room.name = room; p.room.members = 7; p.room.capacity = 16; p.room.joinable = joinable;
}

std::shared_ptr<nf::State> Full() {
    auto state = std::make_shared<nf::State>();
    state->kind = nf::State::Kind::Listed;
    state->code = "4F2A-91CX";
    nf::Incoming first; first.id = "rq_1"; first.name = "Player 9"; first.country = CountryCode::Parsed("IT"); first.sentAt = Now - Hour; first.games = 6; first.lastGameAt = Now - Day;
    nf::Incoming second; second.id = "rq_2"; second.name = "Player 12"; second.country = CountryCode::Parsed("JP"); second.sentAt = Now - 2 * Hour;
    state->incoming = {first, second};
    auto player3 = Pal("3", "Player 3", "FR", 16, 1986, 3, 7, 14, 9);
    InRoom(player3, nf::Status::Fighting, 0, "Player 5's room", true);
    auto player4 = Pal("4", "Player 4", "SE", 5, 412, 1, 5, 3, 3);
    player4.presence.kind = nf::Presence::Kind::InRoom; player4.presence.privateRoom = true;
    auto player5 = Pal("5", "Player 5", "DE", 12, 0, 0, 0, 0, 0);
    InRoom(player5, nf::Status::Idle, -1, "Late night casuals", false);
    auto player6 = Pal("6", "Player 6", "ES", 7, 860, 2, 4, 5, 1);
    player6.presence.kind = nf::Presence::Kind::Online;
    auto player7 = Pal("7", "Player 7", "US", 20, 230, 1, 3, 0, 2);
    player7.presence.kind = nf::Presence::Kind::Online; player7.presence.activity = nf::Activity::OfflinePlay;
    auto player8 = Pal("8", "Player 8", "BR", 3, 86, 0, 0, 1, 1);
    player8.presence.seenAt = Now - 3 * Hour;
    auto player10 = Pal("10", "Player 10", "DE", 9, 0, 0, 0, 0, 0);
    player10.hasMain = false; player10.presence.seenAt = Now - 2 * Day;
    state->friends = {player8, player10, player7, player6, player5, player4, player3};
    state->outgoing = {{"sq_1", "Player 14", Now - 2 * Day}, {"sq_2", "Player 15", Now - 5 * Hour}, {"sq_3", "", Now - Day}};
    state->blocked = {{"bl_1", "Player 16", Now - 9 * Day}};
    return state;
}
}

bool Friends(ui::ShellView& view, const std::string& which) {
    std::shared_ptr<nf::State> state;
    if (which == "full" || which == "unreachable" || which == "offline") state = Full();
    else if (which == "requests") {
        state = Full();
        state->friends.resize(1);
        state->outgoing.clear(); state->blocked.clear();
    } else if (which == "empty") {
        state = std::make_shared<nf::State>();
        state->kind = nf::State::Kind::Listed; state->code = "4F2A-91CX";
    } else if (which == "noaccount") { state = std::make_shared<nf::State>(); state->kind = nf::State::Kind::NoAccount; }
    else if (which == "unavailable" || which == "loading") state = std::make_shared<nf::State>();
    else return false;
    // A service the player trusts, as the other social cases have it.
    auto& id = view.identity;
    id.known = true; id.state = "ready"; id.emberId = "emb1_lab"; id.bridges = {{"brg_1", "https://bridge.example", "Example"}};
    view.unixNow = Now;
    view.preferences.displayName = "Player 1";
    auto& f = view.friends;
    f.bridge = "brg_1";
    if (which == "offline") state->appearOffline = true;
    f.canInvite = which != "requests";
    f.state.value = state;
    f.state.loaded = which != "loading";
    f.state.loading = which == "loading";
    if (which == "unreachable") f.state.error = "bridge_unreachable";
    f.state.revision = f.state.finished = 1;
    return true;
}

void FriendsComing(ui::ShellView& view, int frame) {
    static std::shared_ptr<const nf::State> full;
    auto& f = view.friends;
    if (frame == 0) {
        // The list as it was first read, with nothing waiting: what arrives later is said as a toast.
        full = f.state.value;
        if (!full) return;
        auto first = std::make_shared<nf::State>(*full);
        first->incoming.clear(); first->invites.clear();
        f.state.value = first;
    } else if (frame == 6 && full) {
        // Player 9 and Player 12 ask, then Player 3 invites to their room: one toast a frame.
        auto asked = std::make_shared<nf::State>(*full);
        asked->invites.clear();
        f.state.value = asked; ++f.state.revision; ++f.state.finished;
    } else if (frame == 8 && full) {
        auto invited = std::make_shared<nf::State>(*full);
        nf::Invite invite; invite.id = "iv_1"; invite.friendId = "fr_3"; invite.room.value = std::string(32, 'a'); invite.expiresAt = Now + Hour;
        invited->invites = {invite};
        f.state.value = invited; ++f.state.revision; ++f.state.finished;
    }
}
}
