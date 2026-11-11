// Which points a room shows for a member (docs/design/PLAYER_CARD.md, "Levels on
// the room board"): only a public room's, only from a card the bridge looked up
// by the member's token (a card is public, so whoever has one shows points); the
// local member's own come from the player's own points when no card was looked up.
#include "../ui/RoomPoints.hxx"
#include "../ui/ShellView.hxx"
#include <cstdio>
#include <memory>
#include <string>

namespace {
using namespace sf4e;
using netplay::levels::Card;
using netplay::levels::CardMap;
using netplay::levels::Own;
int failures = 0;
void Check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    std::printf("FAIL: %s\n", what);
}

constexpr room::MemberId Local = 1, Other = 2;

// A public room of two members; the other one's card is the one given, under their token.
struct Room {
    ui::ShellView view;
    Room(bool listed, Card card, bool token = true) {
        view.room.serverOwned = listed;
        view.room.localMember = Local;
        room::Member me;
        me.id = Local;
        room::Member them;
        them.id = Other;
        if (token) them.card = CardToken::Parsed("c1.player_1");
        view.room.members = {me, them};
        auto cards = std::make_shared<CardMap>();
        (*cards)["c1.player_1"] = std::move(card);
        view.memberCards = cards;
    }
    ui::MemberPoints Them() const { return ui::PointsOf(view, view.room.members[1]); }
    ui::MemberPoints Me() const { return ui::PointsOf(view, view.room.members[0]); }
};

Card Shown() {
    Card card;
    card.account = {2100, 2};
    // Not in order: the points decide it, a tie goes to the lower fighter.
    card.fighters = {{9, 100, 0}, {4, 1000, 3}, {7, 100, 0}, {12, 0, 0}};
    return card;
}

void PublicRoomShowsTheCard() {
    const Room room(true, Shown());
    const auto points = room.Them();
    Check(points.known && points.account.points == 2100 && points.account.tier == 2, "a public room's card was not shown with its account points");
    Check(points.fighters.size() == 3 && points.fighters[0].fighter == 4 && points.fighters[1].fighter == 7 && points.fighters[2].fighter == 9,
        "the fighters were not most points first, a tie to the lower fighter, without the one at 0 points");
    const auto four = points.Of(4);
    Check(four && four->points == 1000 && four->tier == 3, "the points and tier of a played fighter were not given");
    Check(!points.Of(5), "a fighter that was not played had points");
    Check(!points.Of(12), "a fighter at 0 points had points");
    Check(!points.Of(-1), "no fighter had points");
}

void PrivateRoomShowsNothing() {
    const Room room(false, Shown());
    const auto points = room.Them();
    Check(!points.known && points.fighters.empty() && !points.Of(4) && points.account.points == 0, "a private room showed points from a card");
}

void NoTokenOrNoCard() {
    const Room noToken(true, Shown(), false);
    Check(!noToken.Them().known, "a member with no card token showed points");
    Room noCard(true, Shown());
    noCard.view.memberCards = std::make_shared<CardMap>();
    Check(!noCard.Them().known, "a member the bridge has no card for yet showed points");
    noCard.view.memberCards = nullptr;
    Check(!noCard.Them().known, "a room with no cards showed points");
}

// A card is public: a player who has played nothing still shows their account,
// at 0, and no fighters.
void NothingPlayedStillShows() {
    Card card;
    const Room room(true, card);
    const auto points = room.Them();
    Check(points.known && points.account.points == 0 && points.fighters.empty() && !points.Of(4), "a card with nothing played was not shown as 0");
}

void OwnSeatFromOwnPoints() {
    Room room(true, Shown());
    Check(!room.Me().known, "the local member had points before their own were read");
    auto& answer = room.view.levels;
    answer.loaded = true;
    answer.value.kind = Own::Kind::Counted;
    answer.value.account.points = 40;
    answer.value.fighters[0].points = 1986;
    answer.value.fighters[0].tier = 3;
    answer.value.fighters[5].points = 3;
    const auto mine = room.Me();
    Check(mine.known && mine.account.points == 40 && mine.fighters.size() == 2 && mine.fighters[0].fighter == 0, "the local member's own points were not used");
    const auto ryu = mine.Of(0);
    Check(ryu && ryu->points == 1986 && ryu->tier == 3, "the local member's points for a fighter were wrong");
    Check(!mine.Of(1), "the local member had points for a fighter they have not played");
    // Their own points are for a public room only, and wait for a Counted answer.
    room.view.room.serverOwned = false;
    Check(!room.Me().known, "the local member's own points showed in a private room");
    room.view.room.serverOwned = true;
    answer.value.kind = Own::Kind::NoAccount;
    Check(!room.Me().known, "the local member's points showed without a Discord account");
    // A card looked up for them wins over the player's own points.
    answer.value.kind = Own::Kind::Counted;
    room.view.room.members[0].card = CardToken::Parsed("c1.player_1");
    Check(room.Me().fighters.size() == 3 && room.Me().Of(4), "a card looked up for the local member was not used");
    // Nobody else borrows the player's own points.
    Card empty;
    (*std::const_pointer_cast<CardMap>(room.view.memberCards))["c1.player_1"] = empty;
    Check(room.Them().known && room.Them().fighters.empty() && room.Them().account.points == 0, "another member showed the local member's points");
}
}

int main() {
    PublicRoomShowsTheCard();
    PrivateRoomShowsNothing();
    NoTokenOrNoCard();
    NothingPlayedStillShows();
    OwnSeatFromOwnPoints();
    if (failures) std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
