// Profile (docs/design/PLAYER_CARD.md): Home's card opens it on Card, LB and RB
// step Card, Fighters, Titles and Icons and wrap, a strip tile opens Fighters
// on that fighter, Select on a fighter makes it the main, the points are read
// when Profile opens and once at start, and the states without points have a
// row the pad can stand on.
#include "public_rooms_journey_support.hxx"
#include "../ui/PlayerCard.hxx"
#include "../ui/ProfilePanel.hxx"
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
void Answer(Journey& j, Own::Kind kind = Own::Kind::Counted, bool shown = true) {
 auto& answer = j.h.view.levels;
 Own own;
 own.kind = kind; own.shown = shown;
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
 const char* order[] = {"card", "fighters", "awards", "awards-icons"};
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

// Fighters lists the played fighters, most points first, and Select makes one the main.
void FightersJourney() {
 Journey j;
 j.h.view.preferences.mainFighter = 0;
 Answer(j);
 j.h.Screen("fighters");
 std::vector<std::string> fighters;
 for (const auto& row : j.rows) if (ui::HasPrefix(row.id, "main-")) fighters.push_back(row.id);
 Check(fighters == std::vector<std::string>({"main-0", "main-5", "main-12", "main-7", "main-20"}), "The Fighters tab does not list the played fighters by points");
 Check(j.row("fighters-unplayed") != nullptr, "The Fighters tab has no row for the fighters not played yet");
 j.h.Choose("main-12");
 j.h.Frame(0, 40);
 const auto* save = Last<commands::SavePreferences>(j.h.actions);
 Check(save && save->preferences.mainFighter == 12, "Select on a fighter did not save it as the main");
 // In a room preferences are locked: the rows are there and cannot change the main.
 j.h.view.canEditPreferences = false; j.h.Frame(0, 2);
 const auto* locked = j.row("main-5");
 Check(locked && !locked->enabled, "A fighter can be made the main while preferences are locked");
}

// The states without levels each leave a row to stand on, on both tabs.
void StatesJourney() {
 struct State { Own::Kind kind; bool answered; const char* words; };
 const State states[] = {{Own::Kind::NoAccount, true, "level.no_account"}, {Own::Kind::Unavailable, true, "level.unavailable"}, {Own::Kind::Counted, false, "level.loading"}};
 for (const auto& state : states) {
  for (const char* screen : {"card", "fighters"}) {
   Journey j;
   if (state.answered) Answer(j, state.kind); else { j.h.view.levels = {}; j.h.view.levels.loading = true; }
   j.h.Screen(screen);
   const auto* words = j.row(ui::ProfileStateRow);
   Check(words && words->label == loc::T(state.words), "A card without levels does not say why");
   if (state.kind == Own::Kind::NoAccount) Check(j.row("discord-connect") != nullptr, "No account has no way to connect Discord");
  }
 }
 // Counted with nothing played: the words, and no fighters.
 Journey j;
 Answer(j);
 j.h.view.levels.value.fighters = {}; ++j.h.view.levels.revision; j.h.Screen("fighters");
 Check(j.row(ui::ProfileStateRow) && !j.row("main-0"), "Nothing played yet lists fighters");
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
  StripJourney(); StripTilesJourney(); FightersJourney(); StatesJourney(); ReadJourney();
  std::cout << "Profile journeys passed.\n";
  return 0;
 } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
