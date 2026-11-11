// Profile (docs/design/PROFILE.md): Home's card and Profile row open it on Card
// (Friends while requests wait), LB and RB step Card, Fighters, Friends and
// Awards and wrap, a strip tile opens Fighters on that fighter, Y and the Edit
// card button open Edit card, Select on a fighter makes it the main, the
// points are read when Profile opens and once at start, and every state
// without points is still a whole card.
#include "public_rooms_journey_support.hxx"
#include "../ui/PlayerCard.hxx"
#include "../ui/ProfilePanel.hxx"
#include "../ui/RowId.hxx"
#include <algorithm>
#include <memory>
namespace {
using netplay::levels::Own;

std::size_t Reads(const Journey& j) {
 return static_cast<std::size_t>(std::count_if(j.h.actions.begin(), j.h.actions.end(), [](const ShellAction& a) { return TournamentIs(a, Op::LevelsMe); }));
}

// The newest levels read sent, or null.
const netplay::tournament::Command* LastRead(const Journey& j) {
 for (auto it = j.h.actions.rbegin(); it != j.h.actions.rend(); ++it) if (TournamentIs(*it, Op::LevelsMe)) return &As<commands::Tournament>(*it)->tournament;
 return nullptr;
}

void Play(Own& own, int fighter, std::uint64_t games) {
 auto& l = own.fighters[fighter];
 l.games = games; l.wins = games / 2; l.points = l.games + l.wins; l.tier = 0; l.next = 5000; l.since = 1700000000;
}

// The points the runtime has: Ryu (0) with the most, and four others.
void Answer(Journey& j, Own::Kind kind = Own::Kind::Counted) {
 auto& answer = j.h.view.levels;
 Own own;
 own.kind = kind;
 if (kind == Own::Kind::Counted) { Play(own, 0, 600); Play(own, 5, 300); Play(own, 12, 200); Play(own, 7, 90); Play(own, 20, 40); }
 answer.value = own; answer.loaded = true; answer.loading = false; ++answer.revision; ++answer.finished;
 j.h.Frame(0, 2);
}

// The service the interface can use, as a first visit to Home leaves it.
void Usable(Journey& j) {
 j.h.Screen("home"); j.h.Frame(0, 2); j.answerIdentityUntilIdle();
}

// Home's card opens Profile on Card; LB and RB step the strip, wrapping at both ends; Back is Home.
void StripJourney() {
 Journey j;
 Answer(j);
 j.h.Screen("home"); j.h.FocusOn("card"); j.h.Press(MenuInput::Select);
 Check(j.h.shell.Navigation().Screen() == "card", "Home's card did not open Profile on Card");
 const char* order[] = {"card", "fighters", "friends", "awards"};
 for (int i = 1; i <= 4; ++i) {
  j.h.Press(MenuInput::NextTab);
  Check(j.h.shell.Navigation().Screen() == order[i % 4], "RB did not step to the next Profile tab");
 }
 for (int i = 3; i >= 0; --i) {
  j.h.Press(MenuInput::PrevTab);
  Check(j.h.shell.Navigation().Screen() == order[i], "LB did not step to the previous Profile tab");
 }
 j.h.Press(MenuInput::Back);
 Check(j.h.shell.Navigation().Screen() == "home", "Back from a Profile tab did not return to Home");
 // A stick's LP and HP step the strip as LB and RB.
 j.h.Screen("card");
 j.h.Press(ControllerButtons(0x400, input::PadDirectInput, input::MenuButtons{}.heavyPunch, {}));
 Check(j.h.shell.Navigation().Screen() == "fighters", "A stick's HP did not step to Fighters");
}

// The strip's tiles are rows in the order the card says; Select on one opens Fighters on that fighter.
void StripTilesJourney() {
 Journey j;
 j.h.view.preferences.mainFighter = 0;
 Answer(j);
 j.h.Screen("card");
 // The main, then the two highest others.
 Check(j.row(ui::CardFighterRow(0)) && j.row(ui::CardFighterRow(5)) && j.row(ui::CardFighterRow(12)), "The strip is not the main and the two highest others");
 Check(!j.row(ui::CardFighterRow(7)), "The strip has a fourth fighter");
 j.h.Choose(ui::CardFighterRow(5).c_str());
 Check(j.h.shell.Navigation().Screen() == "fighters" && j.h.shell.Navigation().Focus() == "main-5", "A strip tile did not open Fighters on its fighter");
 j.h.Press(MenuInput::Back);
 Check(j.h.shell.Navigation().Screen() == "home", "Back from Fighters opened from the strip did not return to Home");
}

// Fighters lists every fighter: the played ones by points, then the rest in roster order; Select makes any of them, played or not, the main.
void FightersJourney() {
 Journey j;
 j.h.view.preferences.mainFighter = 0;
 Answer(j);
 j.h.Screen("fighters");
 std::vector<int> fighters;
 for (const auto& row : j.rows) if (const auto id = ui::RowKey<int>(row.id, "main-")) fighters.push_back(*id);
 // The main (Ryu, also the most played) first, the other four played by points, then the rest as the roster shows them.
 std::vector<int> expected = {0, 5, 12, 7, 20};
 for (const int id : selection::RosterDisplayOrder) if (std::find(expected.begin(), expected.end(), id) == expected.end()) expected.push_back(id);
 Check(fighters == expected, "The Fighters tab does not list the played fighters by points, then the rest in roster order");
 Check(fighters.size() == static_cast<std::size_t>(selection::FighterCount), "The Fighters tab does not list every fighter");
 // A played fighter becomes the main.
 j.h.shell.Navigation().Focus("main-12", j.rows);
 j.h.Press(MenuInput::Select);
 j.h.Frame(0, 40);
 const auto* save = Last<commands::SavePreferences>(j.h.actions);
 Check(save && save->preferences.mainFighter == 12, "Select on a fighter did not save it as the main");
 // So does one never played: its tile is there at 0 points.
 const auto* unplayed = j.row("main-3");
 Check(unplayed && unplayed->enabled, "A fighter not played cannot be chosen");
 j.h.shell.Navigation().Focus("main-3", j.rows);
 j.h.Press(MenuInput::Select);
 j.h.Frame(0, 40);
 save = Last<commands::SavePreferences>(j.h.actions);
 Check(save && save->preferences.mainFighter == 3, "Select on a fighter not played did not save it as the main");
 // In a room preferences are locked: the rows are there and cannot change the main.
 j.h.view.canEditPreferences = false; j.h.Frame(0, 2);
 const auto* locked = j.row("main-5");
 Check(locked && !locked->enabled, "A fighter can be made the main while preferences are locked");
 const auto* lockedUnplayed = j.row("main-3");
 Check(lockedUnplayed && !lockedUnplayed->enabled, "A fighter not played can be made the main while preferences are locked");
}

// The main leads its own group: an unplayed main first among the rest, a played one first among the played.
void FightersMainJourney() {
 Journey j;
 j.h.view.preferences.mainFighter = 20;
 Answer(j);
 j.h.Screen("fighters");
 std::vector<int> fighters;
 for (const auto& row : j.rows) if (const auto id = ui::RowKey<int>(row.id, "main-")) fighters.push_back(*id);
 Check(fighters.size() > 5 && fighters[0] == 20 && fighters[1] == 0, "A played main does not lead the played fighters");
 j.h.view.preferences.mainFighter = 42; j.h.Frame(0, 2);
 j.h.Screen("fighters");
 fighters.clear();
 for (const auto& row : j.rows) if (const auto id = ui::RowKey<int>(row.id, "main-")) fighters.push_back(*id);
 Check(fighters.size() > 5 && fighters[5] == 42 && fighters[0] == 0, "An unplayed main does not lead the fighters not played");
}

// Every state without levels is still a whole card: the published card names the state, the main is on the strip at 0 points,
// Edit card is there, and Connect Discord when there is no account. The Fighters tab lists every fighter at 0.
void StatesJourney() {
 struct State { Own::Kind kind; bool answered; const char* error; ui::CardLevels levels; const char* words; };
 const State states[] = {{Own::Kind::NoAccount, true, "", ui::CardLevels::NoAccount, "level.no_account"},
  {Own::Kind::Unavailable, true, "", ui::CardLevels::Unavailable, "level.unavailable"},
  {Own::Kind::Counted, false, "", ui::CardLevels::Pending, "level.loading"},
  {Own::Kind::Counted, false, "bridge_unreachable", ui::CardLevels::Unreachable, "level.unreachable"}};
 for (const auto& state : states) {
  for (const char* screen : {"card", "fighters"}) {
   Journey j;
   if (state.answered) Answer(j, state.kind);
   else { j.h.view.levels = {}; j.h.view.levels.loading = !*state.error; j.h.view.levels.error = state.error; }
   j.h.view.preferences.mainFighter = 3;
   j.h.Screen(screen);
   Check(ui::MenuPlayerCard().levels == state.levels, "The card does not name the state of its points");
   Check(!ui::MenuPlayerCard().fighters.empty() && ui::MenuPlayerCard().fighters.front().fighter == 3 && ui::MenuPlayerCard().fighters.front().points == 0, "A card without levels does not show the main at 0");
   if (std::string(screen) == "card") {
    // The card is still a card: the main is on its strip, and Edit card is there.
    Check(j.row(ui::CardFighterRow(3)) != nullptr && j.row(ui::CardEditRow) != nullptr, "A card without levels has no strip or no Edit card");
   } else {
    // Every fighter is a tile at 0 points whatever the state; the card says why under the tab's strip.
    Check(j.row("main-3") && j.row("main-0") && !j.row("main-0")->info, "A Fighters tab without levels does not list every fighter");
   }
   if (state.kind == Own::Kind::NoAccount) Check(j.row("discord-connect") != nullptr, "No account has no way to connect Discord");
  }
 }
 // Counted with nothing played: the words, and every fighter still a tile.
 Journey j;
 j.h.view.preferences.mainFighter = 0;
 Answer(j);
 j.h.view.levels.value.fighters = {}; ++j.h.view.levels.revision; j.h.Screen("fighters");
 Check(std::string(ui::player_card::NoLevelWords(ui::MenuPlayerCard())) == "level.none" && j.row("main-0") && j.row("main-43"), "Nothing played yet does not list every fighter");
}

// Y and the Edit card row open Edit card, whose rows are the card's, and Back returns to Card; its Title and Icon rows open the awards wall.
void EditCardJourney() {
 Journey j;
 Answer(j);
 j.h.Screen("card");
 j.h.Press(MenuInput::Options);
 Check(j.h.shell.Navigation().Screen() == "card-edit", "Y on Card did not open Edit card");
 j.h.Frame(0, 2);
 for (const char* id : {"name", "country", "main-character", "card-title", "card-icon", "share-steam"}) Check(j.row(id) != nullptr, "Edit card is missing one of the card's rows");
 j.h.Press(MenuInput::Back);
 Check(j.h.shell.Navigation().Screen() == "card", "Back from Edit card did not return to Card");
 j.h.shell.Navigation().Focus(ui::CardEditRow, j.rows);
 j.h.Press(MenuInput::Select);
 Check(j.h.shell.Navigation().Screen() == "card-edit", "Select on Edit card did not open it");
 j.h.Frame(0, 2);
 j.h.shell.Navigation().Focus("card-title", j.rows);
 j.h.Press(MenuInput::Select);
 Check(j.h.shell.Navigation().Screen() == "awards", "Edit card's Title row did not open the awards wall");
 j.h.Press(MenuInput::Back);
 Check(j.h.shell.Navigation().Screen() == "card-edit", "Back from the awards wall did not return to Edit card");
 // In a room nothing on the card changes.
 j.h.view.canEditPreferences = false; j.h.Frame(0, 2);
 const auto* locked = j.row("card-title");
 Check(locked && !locked->enabled && j.row("main-character") && !j.row("main-character")->enabled, "Edit card changes in a room");
}

// Home's Profile row opens Card, or Friends while requests wait, and says so; the Friends tab counts them.
void HomeJourney() {
 Journey j;
 Answer(j);
 j.h.Screen("home");
 j.h.Choose("profile");
 Check(j.h.shell.Navigation().Screen() == "card", "Home's Profile row did not open Card");
 auto state = std::make_shared<netplay::friends::State>();
 state->kind = netplay::friends::State::Kind::Listed;
 state->incoming.resize(2);
 j.h.view.friends.state.value = state; j.h.view.friends.state.loaded = true;
 j.h.Screen("home"); j.h.Frame(0, 2);
 const auto* row = j.row("profile");
 Check(row && row->value == loc::Tf("home.profile_requests", 2) && row->valueHighlighted, "Home's Profile row does not say how many requests wait");
 j.h.Choose("profile");
 Check(j.h.shell.Navigation().Screen() == "friends", "Home's Profile row did not open Friends while requests wait");
 Check(ui::MenuPlayerCard().requests == 2, "The card does not carry the waiting requests");
}

// The levels are read when Profile opens, and once at start for the service in use.
void ReadJourney() {
 Journey j;
 Usable(j);
 Check(Reads(j) == 1, "Home did not read the levels once at start");
 Check(LastRead(j) && LastRead(j)->bridgeId == "brg_1", "The start read does not name the service");
 j.h.Frame(0, 30);
 Check(Reads(j) == 1, "The levels were read again without a visit");
 j.h.Screen("card"); j.h.Frame(0, 2);
 Check(Reads(j) == 2, "Opening Profile did not read the levels");
 // Stepping through the strip is the same visit.
 j.h.Press(MenuInput::NextTab); j.h.Press(MenuInput::NextTab); j.h.Frame(0, 2);
 Check(Reads(j) == 2, "A tab step read the levels again");
 // A read in flight is waited for, and a visit later reads afresh.
 j.h.Press(MenuInput::Back);
 j.h.view.levels.loading = true;
 j.h.Screen("fighters"); j.h.Frame(0, 2);
 Check(Reads(j) == 2, "The levels were read while a read was in flight");
 j.h.view.levels.loading = false; j.h.Frame(0, 2);
 Check(Reads(j) == 3, "A visit that waited for a read in flight did not read afterwards");
 // A refused read is asked again next frame, and once one goes out, no more.
 j.h.Screen("home"); j.h.Frame(0, 2);
 j.h.accept = false; j.h.Screen("card");
 Check(Reads(j) == 4, "Opening Profile did not ask for the levels");
 j.h.accept = true; j.h.Frame();
 Check(Reads(j) == 5, "A read the queue refused was not asked again");
 j.h.Frame(0, 30);
 Check(Reads(j) == 5, "The levels were read again after one went out");
}
}

int main() {
 try {
  StripJourney(); StripTilesJourney(); FightersJourney(); FightersMainJourney(); StatesJourney(); EditCardJourney(); HomeJourney(); ReadJourney();
  std::cout << "Profile journeys passed.\n";
  return 0;
 } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
