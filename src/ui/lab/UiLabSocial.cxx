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

// The cards the runtime would have read for the friends the tab focuses (FriendCardWanted), by friend id: one for each friend with
// points, with how they play, what they wear and their record, which the friend panel's Panel card draws.
std::shared_ptr<const netplay::levels::CardMap> FriendCards(const nf::State& state) {
    namespace pr = netplay::profiles;
    auto cards = std::make_shared<netplay::levels::CardMap>();
    for (const auto& entry : state.friends) {
        if (!entry.hasAccount || !entry.hasMain) continue;
        netplay::levels::Card card;
        card.name = entry.name; card.country = entry.country; card.badge = entry.badge;
        card.hasMain = true; card.main = entry.main.fighter;
        card.hasSince = true; card.since = entry.since;
        card.worn.title = "streak"; card.worn.icon = "rival";
        card.hasStyle = true; card.style.device = pr::Device::Stick; card.style.link = pr::Link::Wired; card.style.region = "euc1"; card.style.looking = pr::LookingRanked;
        card.awards.games = 2210; card.awards.wins = 1124; card.awards.bestStreak = 14; card.awards.peakClass = 6; card.awards.rival = 20; card.awards.opponents = 30;
        card.account = entry.account;
        card.fighters = {{entry.main.fighter, entry.main.points, entry.main.tier}};
        card.relation = netplay::levels::Relation::Friend; card.friendId = entry.id;
        (*cards)[entry.id] = card;
    }
    return cards;
}
}

bool Find(ui::ShellView& view, const std::string& which) {
    namespace pr = netplay::profiles;
    if (which != "recent" && which != "empty") return false;
    view.profilesService = pr::Service::Offered;
    if (which == "empty") return true;
    // The players met lately, newest first: public rooms, hours ago.
    const char* const names[] = {"Player 2", "Player 5", "Player 9"};
    const char* const countries[] = {"GB", "DE", "IT"};
    const int fighters[] = {12, 5, 16};
    for (int i = 0; i < 3; ++i) {
        pr::Recent recent;
        recent.card = std::string("c1.lab_recent") + std::to_string(i); recent.name = names[i]; recent.country = CountryCode::Parsed(countries[i]);
        recent.fighter = fighters[i]; recent.room = i == 1 ? "Late night casuals" : "Player 5's room"; recent.at = Now - (2 + 9 * i) * Hour;
        view.recentOpponents.push_back(recent);
    }
    return true;
}

bool Friends(ui::ShellView& view, const std::string& which) {
    std::shared_ptr<nf::State> state;
    if (which == "full" || which == "unreachable" || which == "offline") state = Full();
    else if (which == "requests") {
        state = Full();
        state->friends.resize(1);
        state->outgoing.clear(); state->blocked.clear();
    } else if (which == "notifications") {
        // What waits in Notifications: Player 2's request, then Player 3's invite to the room they fight in.
        state = Full();
        nf::Incoming request; request.id = "rq_3"; request.name = "Player 2"; request.country = CountryCode::Parsed("GB");
        request.sentAt = Now - Hour / 2; request.games = 3; request.lastGameAt = Now - Hour;
        state->incoming = {request};
        nf::Invite invite; invite.id = "iv_1"; invite.friendId = "fr_3"; invite.room.value = std::string(32, 'a'); invite.expiresAt = Now + Hour;
        state->invites = {invite};
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
    if (!state->friends.empty()) view.memberCards = FriendCards(*state);
    f.state.loaded = which != "loading";
    f.state.loading = which == "loading";
    if (which == "unreachable") f.state.error = "bridge_unreachable";
    f.state.revision = f.state.finished = 1;
    return true;
}

bool Player(ui::ShellView& view, const std::string& which) {
    using netplay::levels::Relation;
    namespace pr = netplay::profiles;
    const bool friendly = which == "friend" || which == "room", blocked = which == "blocked", inRoom = which == "room";
    if (!friendly && !blocked && which != "stranger" && which != "asked" && which != "sent" && which != "nocard" && which != "nobody" && which != "several" &&
        which != "reading" && which != "unreachable" && which != "unsupported") return false;
    // The friends the page reads presence and relations from, on a service the player trusts.
    if (!Friends(view, "full")) return false;
    view.profilesService = which == "unsupported" ? pr::Service::Missing : pr::Service::Offered;
    // Player 3 fights in a joinable public room, Player 6 is online; both are in the friends list.
    const char* name = inRoom ? "Player 3" : friendly ? "Player 6" : blocked ? "Player 16" : "Player 2";
    const char* country = inRoom ? "FR" : friendly ? "ES" : blocked ? "NL" : "GB";
    const int main = inRoom ? 16 : friendly ? 7 : 12;
    auto& p = view.profile;
    p = pr::Profile();
    p.opened = 1;
    p.key = pr::Key::OfCard(friendly ? "c1.lab_friend" : "c1.lab_player2");
    p.token = p.key.value;
    p.known.name = name; p.known.country = CountryCode::Parsed(country); p.known.badge = SkillBadge::OfClass(7, true); p.known.fighter = main;
    p.card.revision = p.card.finished = 1;
    if (which == "reading") { p.state = pr::Profile::State::Reading; p.card.loading = true; return true; }
    if (which == "unreachable") { p.state = pr::Profile::State::Unreachable; p.error = "bridge_unreachable"; return true; }
    if (which == "unsupported") { p.state = pr::Profile::State::Unsupported; p.error = pr::Unavailable; return true; }
    if (which == "nobody") { p.state = pr::Profile::State::Nobody; p.key = pr::Key::OfName("Player 2"); p.token.clear(); return true; }
    if (which == "nocard") { p.state = pr::Profile::State::NoCard; return true; }
    if (which == "several") {
        p.state = pr::Profile::State::Several;
        p.key = pr::Key::OfName("Player 2"); p.token.clear();
        const char* const countries[] = {"GB", "FR", "BR"};
        for (int i = 0; i < 3; ++i) {
            pr::Hit hit;
            hit.card = std::string("c1.lab_hit") + std::to_string(i); hit.name = "Player 2"; hit.country = CountryCode::Parsed(countries[i]);
            hit.hasMain = true; hit.main = static_cast<std::uint8_t>(5 + 7 * i); hit.badge = SkillBadge::OfClass(static_cast<std::uint8_t>(4 + i), true);
            hit.since = Now - (400 + 90 * i) * Day;
            p.hits.push_back(hit);
        }
        return true;
    }
    auto& card = p.card.value;
    p.card.loaded = true;
    p.state = pr::Profile::State::Card;
    card.name = name; card.country = CountryCode::Parsed(country); card.badge = SkillBadge::OfClass(7, true);
    card.hasMain = true; card.main = static_cast<std::uint8_t>(main);
    card.hasSince = true; card.since = Now - 120 * Day;
    card.worn.title = "streak"; card.worn.icon = "rival";
    card.hasStyle = true; card.style.device = pr::Device::Pad; card.style.link = pr::Link::Wired; card.style.region = "euc1"; card.style.looking = pr::LookingRanked | pr::LookingTeach;
    card.awards.games = 2840; card.awards.wins = 1164; card.awards.bestStreak = 12; card.awards.peakClass = 8; card.awards.rival = 50; card.awards.opponents = 50;
    card.account = {4012, 2};
    card.fighters = {{static_cast<std::uint8_t>(main), 1986, 3}, {5, 860, 2}, {9, 412, 1}, {3, 120, 0}};
    card.hasVersus = true; card.versus = {3, 5, Now - 2 * Hour};
    card.relation = friendly ? Relation::Friend : which == "asked" ? Relation::Received : which == "sent" ? Relation::Sent : blocked ? Relation::Blocked : Relation::None;
    if (friendly) card.friendId = inRoom ? "fr_3" : "fr_6";
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
