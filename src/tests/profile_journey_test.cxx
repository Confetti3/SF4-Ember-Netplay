// Profile (docs/design/PROFILE.md): Home's card and Profile row open it on Card
// (Friends while requests wait), LB and RB step Card, Fighters, Friends and
// Awards and wrap, a strip tile opens Fighters on that fighter, Y and the Edit
// card button open Edit card, Select on a fighter makes it the main, Select on
// an earned award wears its title, its icon or both, the
// points are read when Profile opens and once at start, and every state
// without points is still a whole card.
#include "public_rooms_journey_support.hxx"
#include "../ui/PlayerCard.hxx"
#include "../ui/NameMarks.hxx"
#include "../ui/PlayerPage.hxx"
#include "../ui/CardEditRows.hxx"
#include "../ui/ProfilePanel.hxx"
#include "../ui/RowId.hxx"
#include "../netplay/Awards.hxx"
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
void Counters(Journey& j, std::uint64_t games, std::uint64_t wins, std::uint32_t streak = 0, std::uint8_t peak = 0);
void Usable(Journey& j) {
 j.h.Screen("home"); j.h.Frame(0, 2); j.answerIdentityUntilIdle();
}

// Home's card opens Profile on Card; LB and RB step the strip (Card, Fighters, Sets, Friends, Awards), wrapping at both ends; Back is Home.
void StripJourney() {
 Journey j;
 Answer(j);
 j.h.Screen("home"); j.h.FocusOn("card"); j.h.Press(MenuInput::Select);
 Check(j.h.shell.Navigation().Screen() == "card", "Home's card did not open Profile on Card");
 const char* order[] = {"card", "fighters", "sets", "friends", "awards"};
 for (int i = 1; i <= 5; ++i) {
  j.h.Press(MenuInput::NextTab);
  Check(j.h.shell.Navigation().Screen() == order[i % 5], "RB did not step to the next Profile tab");
 }
 for (int i = 4; i >= 0; --i) {
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
 // The runtime applies the save before the next one can go out.
 if (save) { j.h.view.preferences = save->preferences; j.h.Frame(0, 2); }
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
 for (const char* id : {"name", "country", "main-character", "card-title", "card-icon", "card-pins", "card-device", "card-link", "card-region", "card-looking", "share-steam"}) Check(j.row(id) != nullptr, "Edit card is missing one of the card's rows");
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

// The saves the shell has sent.
std::size_t Saves(const Journey& j) {
 return static_cast<std::size_t>(std::count_if(j.h.actions.begin(), j.h.actions.end(), [](const ShellAction& a) { return Is<commands::SavePreferences>(a); }));
}

// Edit card's choices through the shell: Device steps through its six values, each saved, Connection and Region are shown or hidden,
// Looking for is a page that refuses a third choice with words, the pins page lists the families with a level, refuses a fifth and
// Automatic disables the list, the hint bar names each kind of row once, and in a room every row is disabled.
void EditCardChoicesJourney() {
 Journey j;
 j.h.view.ownLink = NetworkLink::Wired;
 j.h.view.netReport.reported = true; j.h.view.netReport.relay = "euc1";
 Answer(j);
 // Five families reached: matches, wins, streak, rank and (from the five fighters played) cast.
 Counters(j, 120, 3, 3, 5);
 j.h.Screen("card-edit");
 j.h.Frame(0, 2);
 // What a press saved: the preferences it sent, which the runtime then applies. Null when it sent none.
 const auto change = [&](const char* id, unsigned press) {
  j.h.shell.Navigation().Focus(id, j.rows);
  const auto before = Saves(j);
  j.h.Press(press);
  j.h.Frame(0, 40);
  const auto* sent = Saves(j) > before ? Last<commands::SavePreferences>(j.h.actions) : nullptr;
  const auto save = sent ? std::make_shared<commands::SavePreferences>(*sent) : nullptr;
  if (save) { j.h.view.preferences = save->preferences; j.h.Frame(0, 2); }
  return save;
 };

 // Device: Automatic, Pad, Stick, Leverless, Keyboard, Hidden, each step saved, and no step past the end.
 const auto automatic = netplay::profiles::AutomaticDevice(j.h.view.inputDevice.type, j.h.view.inputDevice.connected, j.h.view.gameplayBrand);
 const auto values = ui::card_edit::DeviceValues(automatic);
 Check(j.row("card-device") && j.row("card-device")->value == values[0], "Device does not start on Automatic");
 for (int step = 1; step < 6; ++step) {
  const auto save = change("card-device", MenuInput::Right);
  Check(save && save->preferences.cardDevice == step, "Right did not save the next Device value");
  Check(j.row("card-device") && j.row("card-device")->value == values[step], "Device does not read the value it stepped to");
 }
 Check(!change("card-device", MenuInput::Right), "Device stepped past Hidden");

 // The hint bar: only the arrows' Change on a stepped row, only Choose on a row that opens something, only Change on the switch.
 const auto* device = j.row("card-device");
 Check(device && !ui::MenuPrimaryHint(device) && ui::MenuAdjustHint(*device) == loc::T("menu.change"), "A stepped row's hints are not only the arrows' Change");
 for (const char* id : {"card-pins", "card-looking", "name", "country", "main-character", "card-title", "card-icon"}) {
  const auto* opens = j.row(id);
  Check(opens && opens->hint == loc::T("menu.choose") && ui::MenuAdjustHint(*opens).empty(), "A row that opens something does not say only Choose");
 }
 const auto* steam = j.row("share-steam");
 Check(steam && steam->hint == loc::T("menu.change") && ui::MenuAdjustHint(*steam).empty(), "The switch does not say only Change");

 // Connection and Region are shown or hidden, over what the game measures.
 auto save = change("card-link", MenuInput::Left);
 Check(save && !save->preferences.cardShowLink && j.row("card-link")->value == loc::T("card.hidden"), "Left did not hide the connection");
 save = change("card-link", MenuInput::Right);
 Check(save && save->preferences.cardShowLink && j.row("card-link")->value == loc::Tf("card.shown", loc::T("style.wired")), "Right did not show the connection");
 Check(j.row("card-region") && j.row("card-region")->value == loc::Tf("card.shown", loc::T("network.region_euc1")), "Region does not read the relay region Ember picked");
 save = change("card-region", MenuInput::Left);
 Check(save && !save->preferences.cardShowRegion && j.row("card-region")->value == loc::T("card.hidden"), "Left did not hide the region");

 // Looking for opens a page of six; a third choice is refused with words and saves nothing.
 j.h.shell.Navigation().Focus("card-looking", j.rows);
 j.h.Press(MenuInput::Select);
 Check(j.h.shell.Navigation().Screen() == "card-looking" && j.rows.size() == 6, "Looking for did not open its six choices");
 Check(change("looking:0", MenuInput::Select) && change("looking:3", MenuInput::Select), "Two looking-for choices did not save");
 const auto saved = Saves(j);
 j.h.shell.Navigation().Focus("looking:5", j.rows);
 j.h.Press(MenuInput::Select);
 Check(j.status == loc::T("card.looking_full"), "A third looking-for choice was not refused with words");
 j.h.Frame(0, 40);
 Check(Saves(j) == saved, "A refused third choice was saved");
 j.h.Press(MenuInput::Back);
 Check(j.h.shell.Navigation().Screen() == "card-edit" && j.row("card-looking")->value == std::string(loc::T("style.casuals")) + ", " + loc::T("style.practice"),
  "Back from Looking for does not show the choices on the row");

 // Awards on card: Automatic on lists the families disabled; off it starts from four and refuses a fifth.
 j.h.shell.Navigation().Focus("card-pins", j.rows);
 j.h.Press(MenuInput::Select);
 Check(j.h.shell.Navigation().Screen() == "card-pins", "Awards on card did not open its page");
 Check(j.rows.size() == 6 && j.rows[0].id == "pins-auto" && j.rows[0].enabled && j.rows[0].value == loc::T("common.on"), "The pins page is not Automatic and the five families with a level");
 Check(j.row("pin:matches") && !j.row("pin:matches")->enabled && j.row("pin:cast") && !j.row("pin:mastery"), "Automatic does not disable the list, or a family with no level is listed");
 save = change("pins-auto", MenuInput::Select);
 Check(save && save->preferences.cardPins.size() == netplay::profiles::MaxPins, "Automatic off did not choose the four shown");
 Check(j.row("pin:matches") && j.row("pin:matches")->enabled, "The list is disabled with Automatic off");
 std::string fifth;
 for (const auto& row : j.rows) if (row.id.compare(0, 4, "pin:") == 0 && row.value == loc::T("common.off")) fifth = row.id;
 const auto pinned = Saves(j);
 j.h.shell.Navigation().Focus(fifth, j.rows);
 j.h.Press(MenuInput::Select);
 Check(!fifth.empty() && j.status == loc::T("card.pins_full"), "A fifth family was not refused with words");
 j.h.Frame(0, 40);
 Check(Saves(j) == pinned, "A refused fifth family was saved");
 save = change("pins-auto", MenuInput::Select);
 Check(save && save->preferences.cardPins.empty() && !j.row("pin:matches")->enabled, "Automatic on did not clear the choice and disable the list");

 // In a room none of it changes: every row of the pins page, then of Edit card, is disabled with the reason.
 j.h.view.canEditPreferences = false;
 j.h.Frame(0, 2);
 for (const auto& row : j.rows) Check(!row.enabled, "A pins row changes in a room");
 j.h.Press(MenuInput::Back);
 j.h.Frame(0, 2);
 for (const char* id : {"card-pins", "card-looking", "card-device", "card-link", "card-region", "share-steam", "name", "country", "main-character", "card-title", "card-icon"})
  Check(j.row(id) && !j.row(id)->enabled, "An Edit card row changes in a room");
 for (const char* id : {"card-pins", "card-looking", "card-device", "card-link", "card-region"})
  Check(j.row(id)->detail == loc::T("profile.leave_room_to_edit"), "An Edit card row does not say to leave the room");
}

// The account's counters and the two the bridge adds, as a newer points answer sets them.
void Counters(Journey& j, std::uint64_t games, std::uint64_t wins, std::uint32_t streak, std::uint8_t peak) {
 auto& answer = j.h.view.levels;
 answer.value.account.games = games; answer.value.account.wins = wins;
 answer.value.awards.games = games; answer.value.awards.wins = wins; answer.value.awards.bestStreak = streak; answer.value.awards.peakClass = peak;
 ++answer.revision; ++answer.finished;
 j.h.Frame(0, 2);
}

// One Awards tab for titles and icons: a tile for each family; Select on an earned one opens Wear title, Wear icon and Wear both, each saved on the card; the one worn is off; a locked one only says how to earn it and how far along it is.
void AwardsJourney() {
 Journey j;
 Answer(j);
 Counters(j, 120, 3);
 j.h.Screen("awards");
 // One row for each family, and the families reached are the counters' alone.
 Check(j.rows.size() == netplay::awards::CatalogSize, "The wall is not one row for each family");
 const auto* earned = j.row("award:matches");
 Check(earned && earned->choices.size() == 3 && earned->choices[0].id == "title" && earned->choices[1].id == "icon" && earned->choices[2].id == "both", "An earned award does not offer title, icon and both");
 Check(earned && earned->label == loc::T("award.matches.100.title"), "An earned family is not named by its level");
 Check(earned && earned->detail.find(loc::Tf("awards.progress", 120, 500)) != std::string::npos, "An earned family does not say how far the next level is");
 Check(j.row("award:wins") && !j.row("award:wins")->info && j.row("award:cast") && !j.row("award:cast")->info, "A family the counters reach is locked");
 const auto* locked = j.row("award:rank");
 Check(locked && locked->info && locked->choices.empty() && !locked->enabled && locked->label == loc::T("award.rank.name"), "A locked family can be worn or is not named");
 Check(j.row("award:streak") && j.row("award:streak")->info && j.row("award:mastery")->info && j.row("award:friends")->info && j.row("award:rival")->info, "A family the counters do not reach is earned");
 const auto pick = [&](const char* id, int right) {
  j.h.shell.Navigation().Focus(id, j.rows);
  j.h.Press(MenuInput::Select);
  for (int i = 0; i < right; ++i) j.h.Press(MenuInput::Right);
  j.h.Press(MenuInput::Select);
  j.h.Frame(0, 40);
  const auto* sent = Last<commands::SavePreferences>(j.h.actions);
  // A copy, because the frames that follow may add to the actions. The runtime
  // applies a save before the next one can go out.
  const auto save = sent ? std::make_shared<commands::SavePreferences>(*sent) : nullptr;
  if (save) { j.h.view.preferences = save->preferences; j.h.Frame(0, 2); }
  return save;
 };
 auto save = pick("award:matches", 0);
 Check(save && save->preferences.wornTitle == "matches" && save->preferences.wornIcon != "matches", "Wear title did not wear only the title");
 save = pick("award:wins", 1);
 Check(save && save->preferences.wornTitle == "matches" && save->preferences.wornIcon == "wins", "Wear icon did not wear only the icon");
 j.h.Frame(0, 2);
 earned = j.row("award:matches");
 Check(earned && !earned->choices[0].enabled && earned->choices[0].detail == loc::T("awards.detail_worn") && earned->choices[1].enabled && earned->choices[2].enabled, "The title already worn is not off in the popout");
 save = pick("award:matches", 2);
 Check(save && save->preferences.wornTitle == "matches" && save->preferences.wornIcon == "matches", "Wear both did not wear the title and the icon");
 j.h.Frame(0, 2);
 earned = j.row("award:matches");
 Check(earned && !earned->choices[0].enabled && !earned->choices[1].enabled && !earned->choices[2].enabled && earned->valueHighlighted, "An award worn as title and icon still offers to be worn");
 // In a room nothing on the card changes.
 j.h.view.canEditPreferences = false; j.h.Frame(0, 2);
 Check(j.row("award:wins") && !j.row("award:wins")->enabled, "Awards change in a room");
}

// A saved award that is gone from the catalog, or that the counters do not reach, is ignored: Edit card says None.
// One the counters reach shows by its current level's title, and moves up with it.
void WornClaimsJourney() {
 Journey j;
 j.h.view.preferences.wornTitle = "organizer";
 j.h.view.preferences.wornIcon = "rank";
 Answer(j);
 Counters(j, 120, 3);
 j.h.Screen("card-edit");
 Check(j.row("card-title") && j.row("card-title")->value == loc::T("card.none") && j.row("card-icon") && j.row("card-icon")->value == loc::T("card.none"), "A saved award nobody earned is worn");
 j.h.view.preferences.wornTitle = "matches";
 j.h.Frame(0, 2);
 Check(j.row("card-title") && j.row("card-title")->value == loc::T("award.matches.100.title"), "An earned saved award is not worn at its level");
 Counters(j, 600, 3);
 Check(j.row("card-title") && j.row("card-title")->value == loc::T("award.matches.500.title"), "A worn award did not move up with the player");
}

// No Discord account: the wall still lists every family, locked, with the line that says why and the Connect row of the Card tab.
void AwardsStatesJourney() {
 Journey j;
 Answer(j, Own::Kind::NoAccount);
 j.h.Screen("awards");
 Check(j.rows.size() == netplay::awards::CatalogSize + 1, "Without an account the wall is not every family and the way to one");
 Check(j.row("discord-connect") && j.row("award:matches") && j.row("award:matches")->info && j.row("award:matches")->value == loc::T("awards.locked"), "Without an account an award is earned");
 // A service without points lists them locked with no Connect row.
 Answer(j, Own::Kind::Unavailable);
 Check(j.rows.size() == netplay::awards::CatalogSize && !j.row("discord-connect") && j.row("award:matches") && j.row("award:matches")->info, "A service without points does not list the awards locked");
}

// A new answer that reaches levels says each once with its tier; the first answer of a run says nothing.
void AwardNoticesJourney() {
 Journey j;
 Answer(j);
 const std::string said = loc::Tf("awards.notice", "").substr(0, 5);
 // Notifications lists what was said, newest first.
 const auto told = [&](const std::string& words, bool whole) {
  j.h.Screen("notifications"); j.h.Frame(0, 2);
  return std::any_of(j.rows.begin(), j.rows.end(), [&](const auto& r) { return r.id.compare(0, 6, "toast-") == 0 && (whole ? r.detail == words : r.detail.find(words) != std::string::npos); });
 };
 Check(!told(said, false), "The first answer of a run announced an award");
 Counters(j, 120, 3);
 const std::string dot = " \xC2\xB7 ";
 const std::string titles = std::string(loc::T("award.matches.1.title")) + dot + loc::T("tier.ash") + ", " + loc::T("award.matches.100.title") + dot + loc::T("tier.copper") +
  ", " + loc::T("award.wins.1.title") + dot + loc::T("tier.ash");
 Check(told(loc::Tf("awards.notice", titles), true), "A new answer did not announce the levels it reached");
}

// Where one press from `from` leaves the focus.
std::string Moved(Journey& j, const std::string& from, unsigned direction) {
 j.h.shell.Navigation().Prefer(from); j.h.Frame(0, 2);
 j.h.Press(direction);
 return j.h.shell.Navigation().Focus();
}

// What is drawn in two dimensions moves in two: Left and Right along a line of
// tiles, Up and Down to the tile above or below, at 1280x960 and at 1280x720,
// where the card is compact. The facts drawn beside the points take no focus.
void SpatialJourney() {
 for (const float height : {960.f, 720.f}) {
  Journey j;
  j.h.imgui.io.DisplaySize = ImVec2(1280, height);
  j.h.view.preferences.mainFighter = 0;
   Answer(j);
  // Card: Edit card under the hero, the strip's three tiles in a line beside it.
  j.h.Screen("card"); j.h.Frame(0, 2);
  const std::string ryu = ui::CardFighterRow(0), ken = ui::CardFighterRow(5), third = ui::CardFighterRow(12);
  Check(j.rows.size() == 5, "The card takes the focus somewhere other than its strip, Edit card and Fighter select");
  Check(Moved(j, ui::CardEditRow, MenuInput::Down) == ui::FighterSelectRow && Moved(j, ui::FighterSelectRow, MenuInput::Up) == ui::CardEditRow, "Up and Down do not go between Edit card and Fighter select");
  Check(Moved(j, ui::CardEditRow, MenuInput::Right) == ryu && Moved(j, ryu, MenuInput::Left) == ui::CardEditRow, "Left and Right do not go between Edit card and the strip");
  Check(Moved(j, ryu, MenuInput::Right) == ken && Moved(j, ken, MenuInput::Right) == third && Moved(j, third, MenuInput::Left) == ken, "Left and Right do not step along the strip");
  Check(Moved(j, ken, MenuInput::Down) == ken && Moved(j, ken, MenuInput::Up) == ken, "Up or Down steps along the strip as a list");
  // Fighters: the played tiles on a line, the rest in lines under them.
  j.h.Screen("fighters"); j.h.Frame(0, 2);
  const auto fighter = [&](std::size_t i) { return j.rows.at(i).id; };
  Check(Moved(j, fighter(0), MenuInput::Right) == fighter(1) && Moved(j, fighter(1), MenuInput::Left) == fighter(0), "Left and Right do not step along the fighters");
  Check(Moved(j, fighter(1), MenuInput::Down) == fighter(6) && Moved(j, fighter(6), MenuInput::Up) == fighter(1), "Down from a played fighter is not the tile under it");
  Check(Moved(j, fighter(6), MenuInput::Right) == fighter(7), "Right does not step along the fighters not played");
  // Awards: four across.
  j.h.Screen("awards"); j.h.Frame(0, 2);
  const auto award = [&](std::size_t i) { return j.rows.at(i).id; };
  Check(Moved(j, award(0), MenuInput::Right) == award(1) && Moved(j, award(1), MenuInput::Down) == award(5) && Moved(j, award(5), MenuInput::Up) == award(1), "The awards wall does not move as a grid");
  // Home: the card beside the list is a column of its own.
  j.h.Screen("home"); j.h.Frame(0, 2);
  std::vector<std::string> list;
  for (const auto& row : j.rows) if (row.id != "card") list.push_back(row.id);
  Check(Moved(j, list[1], MenuInput::Right) == "card" && Moved(j, "card", MenuInput::Left) == list.front(), "Left and Right do not go between Home's list and its card");
  Check(Moved(j, "card", MenuInput::Up) == "card" && Moved(j, "card", MenuInput::Down) == "card", "Up or Down on Home's card steps into the list");
  Check(Moved(j, list.back(), MenuInput::Down) == list.front() && Moved(j, list.front(), MenuInput::Up) == list.back(), "Home's list does not go round past the card beside it");
 }
}

namespace pr = sf4e::netplay::profiles;
using Relation = sf4e::netplay::levels::Relation;

// The friends the page reads relations from, listed on the service the journey trusts.
void Friends(Journey& j) {
 auto state = std::make_shared<netplay::friends::State>();
 state->kind = netplay::friends::State::Kind::Listed; state->code = "4F2A-91CX";
 j.h.view.friends.bridge = "brg_1"; j.h.view.friends.state.value = state; j.h.view.friends.state.loaded = true;
 ++j.h.view.friends.state.revision; ++j.h.view.friends.state.finished;
}

// The runtime's answer to the profile open: a card for the token, with this relation.
void Card(Journey& j, const char* token, Relation relation, const char* name = "Player 2") {
 auto& p = j.h.view.profile;
 p = pr::Profile();
 p.opened = 1; p.key = pr::Key::OfCard(token); p.token = token; p.known.name = name;
 p.state = pr::Profile::State::Card; p.card.loaded = true;
 auto& card = p.card.value;
 card.name = name; card.relation = relation; card.hasMain = true; card.main = 12; card.fighters = {{12, 300, 1}};
 ++p.card.revision; ++p.card.finished;
 j.h.view.profilesService = pr::Service::Offered;
 j.h.Frame(0, 2);
}

std::size_t Opens(const Journey& j) {
 return static_cast<std::size_t>(std::count_if(j.h.actions.begin(), j.h.actions.end(), [](const ShellAction& a) { return Is<commands::ProfileOpen>(a); }));
}
std::size_t Closes(const Journey& j) {
 return static_cast<std::size_t>(std::count_if(j.h.actions.begin(), j.h.actions.end(), [](const ShellAction& a) { return Is<commands::ProfileClose>(a); }));
}

// Another player's profile (docs/design/PROFILE.md): opening it asks the runtime once and shows it at once with what the opener
// knew; the card arrives and the page opens on the Primary; LB and RB step its four tabs; A on the Primary sends a friend request
// with their token; Back goes where it was opened from and tells the runtime it was left.
void PlayerJourney() {
 Journey j;
 Usable(j);
 Friends(j);
 pr::Known known;
 known.name = "Player 2"; known.country = CountryCode::Parsed("GB");
 j.h.shell.OpenPlayer(pr::Key::OfCard("c1.player2"), known);
 j.h.Frame(0, 2);
 Check(j.h.shell.Navigation().Screen() == "player-card", "Opening a player did not open their Card tab");
 Check(Opens(j) == 1, "Opening a player did not ask the runtime once");
 const auto* open = Last<commands::ProfileOpen>(j.h.actions);
 Check(open && open->bridge == "brg_1" && open->key == pr::Key::OfCard("c1.player2") && open->known.name == "Player 2", "The open does not name the service, the token and what the opener knew");
 j.h.Frame(0, 30);
 Check(Opens(j) == 1, "The open was asked again without a reason");
 // Reading: the Primary is there and cannot be pressed.
 const auto* reading = j.row(ui::player_page::PrimaryRow);
 Check(reading && !reading->enabled && reading->label == loc::T("profile.reading_button"), "A profile being read does not have a disabled Reading Primary");
 // The card arrives: Add friend, and the page opens on it.
 Card(j, "c1.player2", Relation::None);
 const auto* add = j.row(ui::player_page::PrimaryRow);
 Check(add && add->enabled && add->label == loc::T("friends.add_member"), "A stranger's card does not have Add friend as its Primary");
 Check(j.h.shell.Navigation().Focus() == ui::player_page::PrimaryRow, "The page did not open with the focus on the Primary");
 // LB and RB step the four tabs and wrap.
 const char* order[] = {"player-card", "player-fighters", "player-sets", "player-awards"};
 for (int i = 1; i <= 4; ++i) {
  j.h.Press(MenuInput::NextTab);
  Check(j.h.shell.Navigation().Screen() == order[i % 4], "RB did not step to the next tab of their profile");
 }
 j.h.Press(MenuInput::PrevTab);
 Check(j.h.shell.Navigation().Screen() == "player-awards", "LB did not step back through their profile's tabs");
 j.h.Press(MenuInput::NextTab);
 // A on Add friend asks the bridge by their token, naming them as their label.
 j.h.FocusOn(ui::player_page::PrimaryRow);
 j.h.Press(MenuInput::Select);
 const auto* request = Last<commands::FriendRequest>(j.h.actions);
 Check(request && request->card == "c1.player2" && request->label == "Player 2" && !request->mutual && request->bridge == "brg_1", "A on Add friend did not send their token with their name");
 // Back returns to Home and leaves the profile.
 Check(Closes(j) == 0, "The profile was closed while it was on show");
 j.h.Press(MenuInput::Back);
 j.h.Frame(0, 2);
 Check(j.h.shell.Navigation().Screen() == "home", "Back from their profile did not return to where it was opened from");
 Check(Closes(j) == 1, "Leaving their profile did not tell the runtime");
}

// They asked first: the Primary is Accept request, and it sends the same request, marked mutual.
void PlayerAcceptJourney() {
 Journey j;
 Usable(j);
 Friends(j);
 pr::Known known;
 known.name = "Player 2";
 j.h.shell.OpenPlayer(pr::Key::OfCard("c1.player2"), known);
 Card(j, "c1.player2", Relation::Received);
 const auto* accept = j.row(ui::player_page::PrimaryRow);
 Check(accept && accept->enabled && accept->label == loc::T("friends.accept_member"), "A request received does not make Accept request the Primary");
 j.h.Press(MenuInput::Select);
 const auto* request = Last<commands::FriendRequest>(j.h.actions);
 Check(request && request->card == "c1.player2" && request->mutual, "A on Accept request did not send a mutual request with their token");
}

// A profile opened from a profile replaces it: Back goes to where the first came from. A name several players go by opens the
// pick list, and A on one opens that player; the player's own name opens their own Profile.
void PlayerReplaceJourney() {
 Journey j;
 Usable(j);
 Friends(j);
 j.h.view.preferences.displayName = "Player 1";
 pr::Known known;
 known.name = "Player 2";
 j.h.shell.OpenPlayer(pr::Key::OfCard("c1.player2"), known);
 Card(j, "c1.player2", Relation::None);
 known.name = "Player 5";
 j.h.shell.OpenPlayer(pr::Key::OfCard("c1.player5"), known);
 j.h.Frame(0, 2);
 Check(j.h.shell.Navigation().Screen() == "player-card" && Opens(j) == 2, "A profile opened from a profile did not ask for the new one");
 j.h.Press(MenuInput::Back);
 Check(j.h.shell.Navigation().Screen() == "home", "Back from a profile that replaced another did not return to where the first came from");
 // A name with several players behind it: the pick list, newest first as the search gave them.
 j.h.shell.OpenPlayer(pr::Key::OfName("Player 7"), pr::Known());
 j.h.Frame(0, 2);
 auto& p = j.h.view.profile;
 p = pr::Profile();
 p.opened = 2; p.key = pr::Key::OfName("Player 7"); p.state = pr::Profile::State::Several;
 for (int i = 0; i < 2; ++i) {
  pr::Hit hit;
  hit.card = std::string("c1.hit") + std::to_string(i); hit.name = "Player 7"; hit.country = CountryCode::Parsed(i ? "FR" : "GB");
  p.hits.push_back(hit);
 }
 j.h.Frame(0, 3);
 Check(j.h.shell.Navigation().Screen() == "player-pick", "Several players with one name did not open the pick list");
 Check(j.row("player-hit:0") && j.row("player-hit:1"), "The pick list is not a row for each player");
 j.h.Choose("player-hit:1");
 Check(j.h.shell.Navigation().Screen() == "player-card", "A on a player in the pick list did not open their profile");
 const auto* open = Last<commands::ProfileOpen>(j.h.actions);
 Check(open && open->key == pr::Key::OfCard("c1.hit1"), "A on a player in the pick list did not ask for that player's card");
 j.h.Press(MenuInput::Back);
 Check(j.h.shell.Navigation().Screen() == "home", "Back from a profile chosen from the pick list did not return to where the list was opened from");
 // The player's own name is their own Profile.
 j.h.shell.OpenPlayer(pr::Key::OfName("player 1"), pr::Known());
 j.h.Frame(0, 2);
 Check(j.h.shell.Navigation().Screen() == "card", "The player's own name did not open their own Profile on Card");
}

// A on the own card's Most played opens that player's profile by the token the points answer carries, and All sets opens the own Sets
// tab, which lists the archive's sets under the player's own name from the profile's slot.
void MostPlayedAndSetsJourney() {
 Journey j;
 Answer(j);
 j.h.view.preferences.displayName = "Player 1";
 auto& rival = j.h.view.levels.value;
 rival.hasMostPlayed = true;
 rival.mostPlayed.card = "c1.rival"; rival.mostPlayed.name = "Player 9"; rival.mostPlayed.won = 41; rival.mostPlayed.lost = 33;
 Usable(j);
 j.h.Screen("card"); j.h.Frame(0, 2);
 j.h.Choose(ui::player_card::MostPlayedRow);
 Check(j.h.shell.Navigation().Screen() == "player-card", "Most played did not open that player's profile");
 const auto* open = Last<commands::ProfileOpen>(j.h.actions);
 Check(open && open->key == pr::Key::OfCard("c1.rival") && open->known.name == "Player 9", "Most played did not ask for that player by their token");
 j.h.Press(MenuInput::Back);
 Check(j.h.shell.Navigation().Screen() == "card", "Back from the most played opponent's profile did not return to the card");
 // All sets: the own Sets tab, with its list asked for by the player's own name from the profile's slot.
 j.h.Choose(ui::player_card::AllSetsRow);
 j.h.Frame(0, 2);
 Check(j.h.shell.Navigation().Screen() == "sets", "All sets did not open the Sets tab");
 const auto* list = Last<commands::ArchiveList>(j.h.actions);
 Check(list && list->profile && list->filter.name == "Player 1", "The own Sets tab did not ask for the player's own sets from the profile's slot");
 Check(j.row("archive-filters") && j.row("archive-note"), "The own Sets tab has not its Filters row and the note about the name");
 Check(!j.row("card-edit"), "The own Sets tab shows the card's own rows");
}

// Home's Profile row opens Card; requests waiting are counted once on Home, on the News row that opens Notifications
// where they are answered, and the Friends tab counts them.
void HomeJourney() {
 Journey j;
 Answer(j);
 j.h.Screen("home");
 j.h.Choose("profile");
 Check(j.h.shell.Navigation().Screen() == "card", "Home's Profile row did not open Card");
 auto state = std::make_shared<netplay::friends::State>();
 state->kind = netplay::friends::State::Kind::Listed; state->code = "4F2A-91CX";
 state->incoming.resize(2);
 state->incoming[0].id = "rq_1"; state->incoming[0].name = "Player 2";
 state->incoming[1].id = "rq_2"; state->incoming[1].name = "Player 3";
 j.h.view.friends.bridge = "brg_1"; j.h.view.friends.state.value = state; j.h.view.friends.state.loaded = true;
 j.h.Screen("home"); j.h.Frame(0, 2);
 const auto* profile = j.row("profile");
 const auto* news = j.row("news");
 Check(profile && profile->value.empty(), "Home's Profile row counts the requests the News row counts");
 Check(news && news->value == loc::Tf("notifications.waiting", 2) && news->valueHighlighted, "Home's News row does not say how many requests wait");
 j.h.Choose("profile");
 Check(j.h.shell.Navigation().Screen() == "card", "Home's Profile row did not open Card while requests wait");
 Check(ui::MenuPlayerCard().requests == 2, "The card does not carry the waiting requests for the Friends tab");
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
  StripJourney(); StripTilesJourney(); FightersJourney(); FightersMainJourney(); StatesJourney(); EditCardJourney(); EditCardChoicesJourney(); AwardsJourney(); WornClaimsJourney(); AwardsStatesJourney(); AwardNoticesJourney(); HomeJourney(); ReadJourney(); SpatialJourney(); PlayerJourney(); PlayerAcceptJourney(); PlayerReplaceJourney(); MostPlayedAndSetsJourney();
  std::cout << "Profile journeys passed.\n";
  return 0;
 } catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
}
