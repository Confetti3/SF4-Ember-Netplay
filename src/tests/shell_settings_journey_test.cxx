// The settings' journeys that cross screens: the one rule for the settings a
// room leaves open, pad and pointer alike; Retry saving, the shell's own row,
// drawn and reached on every kind of screen; a Settings panel opened from the
// rail of another; a save that goes on while Ember is hidden; and the rows of
// the game features only some games have.
#include "shell_journey_support.hxx"
#include "shell_additional_journeys.hxx"
#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>
namespace {
using Box = std::pair<ImVec2, ImVec2>;
// What the shell drew on its last frame: the rows (SetMenuEntriesProbe) and
// the boxes of what reports them (SetMenuCardProbe), while it is alive.
struct Drawn {
 std::vector<MenuEntry> rows;
 std::map<std::string, Box> boxes;
 Drawn() {
  SetMenuEntriesProbe([this](const std::vector<MenuEntry>& shown) { rows = shown; });
  SetMenuCardProbe([this](const char* id, ImVec2 min, ImVec2 max) { boxes[id] = {min, max}; });
 }
 ~Drawn() { SetMenuEntriesProbe({}); SetMenuCardProbe({}); }
 const MenuEntry* Row(const char* id) const {
  const auto e = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& r) { return r.id == id; });
  return e == rows.end() ? nullptr : &*e;
 }
 bool Open(const char* id) const { const auto* e = Row(id); return e && e->enabled; }
 std::string Value(const char* id) const { const auto* e = Row(id); return e ? e->value : std::string(); }
 // Inside the window, with a size: drawn where the player sees it.
 bool Shown(const char* id) const {
  const auto b = boxes.find(id);
  if (b == boxes.end()) return false;
  const auto size = ImGui::GetIO().DisplaySize;
  return b->second.second.x > b->second.first.x && b->second.second.y > b->second.first.y &&
   b->second.first.x >= 0 && b->second.first.y >= 0 && b->second.second.x <= size.x && b->second.second.y <= size.y;
 }
 ImVec2 Centre(const char* id) const {
  const auto& b = boxes.at(id);
  return ImVec2((b.first.x + b.second.x) * .5f, (b.first.y + b.second.y) * .5f);
 }
};
// The left button pressed at `from`, moved by (dx, dy) and let go.
void Drag(Harness& h, ImVec2 from, float dx, float dy) {
 auto& io = ImGui::GetIO();
 io.AddMousePosEvent(from.x, from.y); h.Frame();
 io.AddMouseButtonEvent(0, true); h.Frame();
 io.AddMousePosEvent(from.x + dx, from.y + dy); h.Frame(); h.Frame();
 io.AddMouseButtonEvent(0, false); h.Frame();
 io.AddMousePosEvent(1, 1); h.Frame();
}
void Click(Harness& h, ImVec2 at) { Drag(h, at, 0, 0); }
std::size_t RoomSaves(const Harness& h, std::size_t from) {
 return static_cast<std::size_t>(std::count_if(h.actions.begin() + static_cast<std::ptrdiff_t>(from), h.actions.end(), [](const ShellAction& a) { return Is<commands::RoomSettings>(a); }));
}
// A room this player has joined, healthy, with its own member.
void InRoom(Harness& h, std::uint64_t epoch) {
 JoinedRoom(h, epoch); h.view.session.generation.room = 1; h.view.session.control = sf4e::netplay::Health::Healthy; h.view.room.host = 1;
 sf4e::room::Member local; local.id = 1; local.name = "Local"; h.view.room.members.push_back(local);
 h.Frame();
}

// The settings a room leaves open change by one rule (ShellView::
// CanEditRoomSettings), whatever changes them: while this player fights, Room
// layout's rows take no Right, its picture's grip drags nothing and the board's
// Hide hides nothing, and nothing is saved; once the fight is over the row and
// the grip both change the layout, and each change is saved.
void RoomSettingsFollowOneRule() {
 using namespace sf4e;
 Harness h; h.Frame();
 InRoom(h, 21);
 h.view.atMainMenu = false; h.view.session.match = netplay::MatchState::Playing; h.Frame();
 Check(!h.view.CanEditRoomSettings(), "A room mid-fight left its settings open");
 Drawn drawn;
 h.Screen("tab-interface/room-layout");
 const std::string column = drawn.Value("arrange-column");
 const auto first = h.actions.size();
 Check(!drawn.Open("arrange-column") && !drawn.Open("arrange-split"), "Room layout's rows were open while the room's settings were closed");
 h.shell.Navigation().Prefer("arrange-column"); h.Frame();
 h.Press(MenuInput::Right); h.Wait(1); h.Frame();
 Check(drawn.Value("arrange-column") == column, "Right changed a closed Room layout row");
 Check(drawn.Shown("##room-column"), "The layout's picture did not show its column grip while closed");
 Drag(h, drawn.Centre("##room-column"), -120, 0); h.Wait(1); h.Frame();
 Check(drawn.Value("arrange-column") == column, "The picture's grip dragged the column while the room's settings were closed");
 // The board's Hide is the same layout.
 h.Screen("room"); h.Frame();
 Check(drawn.Row("room-hide-members") && !drawn.Open("room-hide-members"), "The board's Hide was open while the room's settings were closed");
 h.shell.Navigation().Prefer("room-hide-members"); h.Frame(); h.Press(MenuInput::Select); h.Frame();
 Check(drawn.Row("room-hide-members") && !drawn.Row("room-show-members"), "The board's Hide hid Members while the room's settings were closed");
 Check(RoomSaves(h, first) == 0, "A closed room setting was saved");
 // The fight over (the game still running the watched match): the row and the grip both work, and save.
 h.view.session.match = netplay::MatchState::None;
 h.Screen("tab-interface/room-layout");
 Check(drawn.Open("arrange-column"), "Room layout's rows stayed closed after the fight");
 h.shell.Navigation().Prefer("arrange-column"); h.Frame();
 const auto second = h.actions.size();
 h.Press(MenuInput::Right); h.Wait(1); h.Frame();
 const std::string stepped = drawn.Value("arrange-column");
 Check(stepped != column && RoomSaves(h, second) == 1, "Right on Room layout's column did not change and save it after the fight");
 h.view.preferences = std::get<commands::RoomSettings>(h.actions.back()).preferences; h.Frame();
 const auto third = h.actions.size();
 Drag(h, drawn.Centre("##room-column"), -120, 0); h.Wait(1); h.Frame();
 Check(drawn.Value("arrange-column") != stepped && RoomSaves(h, third) == 1, "The picture's grip did not drag and save the column after the fight");
}

// The retry row a failed save adds to every screen is the shell's own
// (MenuEntry::footer): drawn on screen and reached with the pad on a list
// (the Ember ID, rankings and replays), on a body of its own that knows
// nothing of it (a closed Settings tab, the awards wall), and in a room,
// where it is open for the room's own settings, which the room saves.
void RetrySaveOnPanelScreens() {
 using namespace sf4e;
 const auto reach = [](Harness& h) {
  for (int i = 0; i < 40 && h.shell.Navigation().Focus() != "retry-save"; ++i) h.Press(MenuInput::Down);
  return h.shell.Navigation().Focus() == "retry-save";
 };
 for (const char* screen : {"identity", "rankings", "replays", "tab-access", "awards"}) {
  Harness h; h.Frame();
  h.Screen("tab-access/text"); h.FocusOn("scale"); h.Press(MenuInput::Right);
  h.view.settingsError = "Disk unavailable"; h.Frame(0, 45);
  Drawn drawn;
  h.Screen(screen);
  const std::string where = std::string(" on ") + screen;
  Check(reach(h), ("The pad did not reach Retry saving" + where).c_str());
  h.Frame();
  Check(drawn.Shown("retry-save") && drawn.Open("retry-save"), ("Retry saving was not drawn, or not open," + where).c_str());
  const auto mark = h.actions.size();
  h.Press(MenuInput::Select); h.Frame();
  const bool saved = std::any_of(h.actions.begin() + static_cast<std::ptrdiff_t>(mark), h.actions.end(), [](const ShellAction& a) { return Is<commands::SavePreferences>(a); });
  Check(saved && h.shell.Navigation().Screen() == screen, ("Retry saving did not retry the save" + where).c_str());
 }
 // A room's own settings: their save failed, the retry is open in the room and goes out as the room's request.
 Harness h; h.Frame();
 InRoom(h, 22);
 Drawn drawn;
 h.Screen("tab-interface/room-layout"); h.shell.Navigation().Prefer("arrange-column"); h.Frame();
 h.accept = false; h.Press(MenuInput::Right); h.Wait(1); h.Frame(); h.accept = true;
 Check(reach(h), "The pad did not reach Retry saving after a room setting failed to save");
 h.Frame();
 Check(drawn.Shown("retry-save") && drawn.Open("retry-save"), "Retry saving was not drawn, or not open, for a room setting's failed save");
 // Up leaves it for the panel's rows, and Down comes back.
 h.Press(MenuInput::Up);
 Check(h.shell.Navigation().Focus().compare(0, 8, "arrange-") == 0, "Up from Retry saving did not return to the panel's rows");
 Check(reach(h), "Down from the panel's rows did not return to Retry saving");
 const auto mark = h.actions.size();
 h.Press(MenuInput::Select); h.Frame();
 Check(RoomSaves(h, mark) == 1 && Is<commands::RoomSettings>(h.actions.back(), [](const commands::RoomSettings& s) { return s.preferences.roomColumn == netplay::DefaultRoomColumn + netplay::RoomColumnStep; }),
  "Retry saving did not send the room setting again in the room's own request");
}

// Opening another panel from the rail of the one open replaces it, and its
// tab's focus moves to its head in the same step: Back from Room layout,
// opened from Match HUD's rail, lands on Room layout's head.
void PanelFromRailFocusesHead() {
 Harness h; h.Frame();
 Drawn drawn;
 h.Screen("tab-interface"); h.Choose("tab-interface/match-hud");
 Check(h.shell.Navigation().Screen() == "tab-interface/match-hud", "Select on Match HUD's head did not open it");
 h.Frame();
 Click(h, drawn.Centre("tab-interface/room-layout"));
 Check(h.shell.Navigation().Screen() == "tab-interface/room-layout", "Room layout's head on the rail did not open it");
 h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen() == "tab-interface" && h.shell.Navigation().Focus() == "tab-interface/room-layout",
  "Back from a panel opened on the rail did not land on its head");
 h.Press(MenuInput::Back);
 Check(h.shell.Navigation().Screen() == "home", "Back from the tab did not return Home");
}

// A setting left unsaved when Ember hides saves while it is hidden, and the
// runtime's acknowledgment is taken there too: a new main chosen, Ember
// hidden at once, the save goes out hidden, and once the snapshot holds it
// the main-character screen has returned to Profile before Ember shows again.
void SaveWhileHidden() {
 using namespace sf4e;
 Harness h; h.Frame();
 const auto hidden = [&](float seconds) {
  auto& io = ImGui::GetIO(); io.DeltaTime = seconds; SetMenuInput({0, 0}); ImGui::NewFrame();
  h.shell.Background(h.view, h.view.room, [&](ShellAction a) { h.actions.push_back(a); return h.accept; });
  ImGui::Render();
 };
 const int* order = selection::RosterDisplayOrder;
 const int main = order[2] != h.view.preferences.mainFighter ? order[2] : order[3];
 const std::string portrait = "main-" + std::to_string(main);
 h.Screen("profile"); h.Choose("main-character"); h.FocusOn(portrait.c_str());
 const auto mark = h.actions.size();
 h.Press(MenuInput::Select);
 Check(h.actions.size() == mark && h.shell.Navigation().Screen() == "main-character", "The main was saved before Ember hid");
 hidden(1); hidden(1.f / 60);
 const auto sent = std::find_if(h.actions.begin() + static_cast<std::ptrdiff_t>(mark), h.actions.end(), [](const ShellAction& a) { return Is<commands::SavePreferences>(a); });
 const auto* save = sent != h.actions.end() ? As<commands::SavePreferences>(*sent) : nullptr;
 Check(save && save->preferences.mainFighter == main, "A setting left unsaved was not saved while Ember was hidden");
 Check(h.shell.Navigation().Screen() == "main-character", "The main-character screen closed before the save was acknowledged");
 h.view.preferences = save->preferences; hidden(1.f / 60);
 Check(h.shell.Navigation().Screen() == "profile", "The save's acknowledgment was not taken while Ember was hidden");
 const auto after = h.actions.size();
 h.Frame(0, 40);
 Check(std::none_of(h.actions.begin() + static_cast<std::ptrdiff_t>(after), h.actions.end(), [](const ShellAction& a) { return Is<commands::SavePreferences>(a); }),
  "The save went out again once Ember showed");
}

// A game without a feature (ShellView::settingsFeatures, offersSteamProfile;
// SFV's adapter has none of them) is offered none of its rows: not on the
// Player page or its panel, Profile, Room layout or a member's actions, and
// Sounds and Help, left with no row, have no head on their tab. The pad walks
// each page's rows that are left, top to bottom, and never meets one. With
// every feature, as SF4's adapter has them, every row and head is there.
void RowsFollowTheGame() {
 using namespace sf4e;
 const std::vector<std::string> featureRows = {"lower-input-lag", "lower-keyboard-lag", "faster-display", "borderless", "controllers", "capture", "controls",
  "game-buttons", "game-reads-pads", "keyboard", "ready-sound", "ready-volume", "ready-test", "lobby-music", "arrange-pictures", "shortcut-hint",
  "share-steam", "tab-interface/sounds", "tab-access/help"};
 const std::vector<const char*> screens = {"player", "tab-player/controls", "tab-player/profile", "tab-interface", "tab-interface/room-layout", "tab-access"};
 const auto offer = [](Harness& h, bool every) {
  h.view.settingsFeatures = every ? ember::SettingsFeatures::All() : ember::SettingsFeatures{};
  h.view.offersSteamProfile = every; h.Frame();
 };
 {
  Harness h; h.Frame();
  Drawn drawn;
  const auto shown = [&](const std::string& id) { return drawn.Row(id.c_str()) != nullptr; };
  offer(h, false);
  for (const char* screen : screens) {
   h.Screen(screen);
   const std::string where = std::string(" on ") + screen;
   Check(!drawn.rows.empty(), ("Nothing was left" + where).c_str());
   for (const auto& id : featureRows) Check(!shown(id), ("A game without its feature was offered " + id + where).c_str());
   // Top to bottom by the pad: no row reached is one left out, and the last row is reached.
   for (int i = 0; i < 60; ++i) { const std::string before = h.shell.Navigation().Focus(); h.Press(MenuInput::Up); if (h.shell.Navigation().Focus() == before) break; }
   std::vector<std::string> reached{h.shell.Navigation().Focus()};
   for (int i = 0; i < 60; ++i) { h.Press(MenuInput::Down); if (h.shell.Navigation().Focus() == reached.back()) break; reached.push_back(h.shell.Navigation().Focus()); }
   for (const auto& id : reached)
    Check(std::find(featureRows.begin(), featureRows.end(), id) == featureRows.end(), ("The pad focused " + id + ", a row left out," + where).c_str());
   Check(std::find(reached.begin(), reached.end(), drawn.rows.back().id) != reached.end(), ("The pad did not reach the last row" + where).c_str());
  }
  offer(h, true);
  std::vector<std::string> all;
  // The panels a game without the feature has no head for hold their rows.
  std::vector<const char*> everyScreen = screens;
  everyScreen.insert(everyScreen.end(), {"tab-interface/sounds", "tab-access/help"});
  for (const char* screen : everyScreen) { h.Screen(screen); for (const auto& row : drawn.rows) all.push_back(row.id); }
  for (const auto& id : featureRows) Check(std::count(all.begin(), all.end(), id) > 0, ("With every feature, " + id + " was not offered").c_str());
 }
 // A member's actions, in a room with a peer who shows their Steam profile.
 Harness h; h.Frame();
 InRoom(h, 23);
 room::Member peer; peer.id = 2; peer.name = "Peer"; peer.steamId = SteamId::Parsed("76561200000000000");
 h.view.room.members.push_back(peer); ++h.view.room.revision;
 Drawn drawn;
 const auto steamChoice = [&] {
  h.Screen("room-members");
  const auto* row = drawn.Row("member-2");
  Check(row != nullptr, "The peer's row was not shown");
  return std::any_of(row->choices.begin(), row->choices.end(), [](const MenuChoice& c) { return c.id == "steam-profile"; });
 };
 offer(h, false);
 Check(!steamChoice(), "A game without Steam profiles offered a member's Steam profile");
 offer(h, true);
 Check(steamChoice(), "With Steam profiles, a member's Steam profile was not offered");
 // A room's More… opens the controllers only for a game with a button layout
 // to show; SFV's buttons are the game's own Options.
 const auto controllersChoice = [&] {
  h.Screen("room"); h.Frame(0, 2);
  const auto* more = drawn.Row("room-more");
  return more && std::any_of(more->choices.begin(), more->choices.end(), [](const MenuChoice& c) { return c.id == "room-controls"; });
 };
 offer(h, false);
 Check(!controllersChoice(), "A game without a pad layer offered the controllers in a room's More");
 offer(h, true);
 Check(controllersChoice(), "With a pad layer, a room's More did not offer the controllers");
}
// Look's Background artwork row is the game's (ShellView::offersMenuArtwork):
// SF4's Look keeps TV mode alone, and a game that draws its own artwork gets
// the row after it, showing the game's choice. Left and Right ask the game
// (no runtime command, no saved preference); a press that changes nothing
// sends nothing, and the row shows what the game says, not the press.
void MenuArtworkRow() {
 using namespace sf4e;
 Harness h; h.Frame();
 Check(!ShellView{}.offersMenuArtwork && !h.view.offersMenuArtwork, "Background artwork was offered by default");
 Drawn drawn;
 h.Screen("tab-interface/look");
 Check(drawn.rows.size() == 1 && drawn.rows[0].id == "tv", "A game without its own artwork was offered the row");
 h.view.offersMenuArtwork = true; h.Frame();
 const auto* row = drawn.Row("menu-artwork");
 Check(drawn.rows.size() == 2 && drawn.rows[1].id == "menu-artwork" && row && row->label == loc::T("settings.menu_artwork") &&
  row->value == loc::T("common.off") && row->detail == loc::T("settings.menu_artwork_detail"), "Look's Background artwork row is missing or out of place");
 h.view.menuArtworkShown = true; h.Frame();
 Check(drawn.Value("menu-artwork") == loc::T("common.on"), "The Background artwork row did not show the game's choice");
 const auto before = h.actions.size();
 h.FocusOn("menu-artwork");
 h.Press(MenuInput::Right);
 Check(h.actions.size() == before, "Right on Background artwork already shown asked again");
 h.Press(MenuInput::Left);
 Check(h.actions.size() == before + 1 && test::Is<ui::ShowMenuArtwork>(h.actions.back(), [](const ui::ShowMenuArtwork& a) { return !a.shown; }), "Left on Background artwork did not ask the game to hide it");
 h.Frame();
 Check(drawn.Value("menu-artwork") == loc::T("common.on"), "The Background artwork row changed before the game kept the choice");
 h.view.menuArtworkShown = false; h.Frame();
 h.Press(MenuInput::Right);
 Check(h.actions.size() == before + 2 && test::Is<ui::ShowMenuArtwork>(h.actions.back(), [](const ui::ShowMenuArtwork& a) { return a.shown; }), "Right on Background artwork did not ask the game to show it");
 h.view.offersMenuArtwork = false; h.Frame();
 Check(!drawn.Row("menu-artwork"), "The Background artwork row stayed for a game that does not draw its own");
}
// Look's Screen mode row is the game's (ShellView::offersScreenMode), after
// Background artwork where that is offered: SF4's Look has neither. The row
// shows the game's saved choice (Game's setting, Window, Borderless); Left and
// Right step through the three and stop at the ends, ask the game with the
// new number (no runtime command, no saved preference), and a press that
// changes nothing sends nothing.
void ScreenModeRow() {
 using namespace sf4e;
 Harness h; h.Frame();
 Check(!ShellView{}.offersScreenMode && !h.view.offersScreenMode && ShellView{}.screenMode == 0, "Screen mode was offered by default");
 Drawn drawn;
 h.Screen("tab-interface/look");
 Check(drawn.rows.size() == 1 && !drawn.Row("screen-mode"), "A game without a screen mode of its own was offered the row");
 h.view.offersScreenMode = true; h.Frame();
 const auto* row = drawn.Row("screen-mode");
 Check(drawn.rows.size() == 2 && drawn.rows[1].id == "screen-mode" && row && row->label == loc::T("settings.screen_mode") &&
  row->value == loc::T("settings.screen_mode_game") && row->detail == loc::T("settings.screen_mode_detail"), "Look's Screen mode row is missing or out of place");
 h.view.offersMenuArtwork = true; h.Frame();
 Check(drawn.rows.size() == 3 && drawn.rows[1].id == "menu-artwork" && drawn.rows[2].id == "screen-mode", "Screen mode did not follow Background artwork");
 h.view.screenMode = 1; h.Frame();
 Check(drawn.Value("screen-mode") == loc::T("settings.screen_mode_window"), "The Screen mode row did not show Window");
 h.view.screenMode = 2; h.Frame();
 Check(drawn.Value("screen-mode") == loc::T("player.borderless"), "The Screen mode row did not show Borderless");
 const auto before = h.actions.size();
 h.FocusOn("screen-mode");
 h.Press(MenuInput::Right);
 Check(h.actions.size() == before, "Right on Screen mode at Borderless asked again");
 h.Press(MenuInput::Left);
 Check(h.actions.size() == before + 1 && test::Is<ui::SetScreenMode>(h.actions.back(), [](const ui::SetScreenMode& a) { return a.mode == 1; }), "Left on Screen mode did not ask the game for Window");
 h.Frame();
 Check(drawn.Value("screen-mode") == loc::T("player.borderless"), "The Screen mode row changed before the game kept the choice");
 h.view.screenMode = 0; h.Frame();
 h.Press(MenuInput::Left);
 Check(h.actions.size() == before + 1, "Left on Screen mode at Game's setting asked again");
 h.Press(MenuInput::Right);
 Check(h.actions.size() == before + 2 && test::Is<ui::SetScreenMode>(h.actions.back(), [](const ui::SetScreenMode& a) { return a.mode == 1; }), "Right on Screen mode did not ask the game for Window");
 h.view.offersScreenMode = false; h.Frame();
 Check(!drawn.Row("screen-mode"), "The Screen mode row stayed for a game that does not offer it");
}
// Interface size steps below 1.00x with Left (pad or keys) down to 0.70x, where it stops; every step shows as
// its value, and the size saved is the one the preferences accept.
void InterfaceSizeBelowOne() {
 using namespace sf4e;
 Harness h; h.Frame();
 Drawn drawn;
 h.Screen("tab-access/text"); h.FocusOn("scale");
 Check(drawn.Value("scale") == "1.00x", "Interface size does not start at 1.00x");
 h.Press(MenuInput::Left);
 Check(drawn.Value("scale") == "0.95x", "Left from 1.00x did not step to 0.95x");
 for (int i = 0; i < 8; ++i) h.Press(MenuInput::Left);
 Check(drawn.Value("scale") == "0.70x", "Interface size did not stop at 0.70x");
 h.Frame(0, 45);
 Check(Is<commands::SavePreferences>(h.actions.back(), [](const commands::SavePreferences& s) { return s.preferences.interfaceScale == netplay::MinInterfaceScale && s.preferences.Valid(); }),
  "The least Interface size was not saved as a valid preference");
 // Right steps it back up.
 h.view.preferences = std::get<commands::SavePreferences>(h.actions.back()).preferences; h.Frame();
 h.Press(MenuInput::Right);
 Check(drawn.Value("scale") == "0.75x", "Right from 0.70x did not step to 0.75x");
 Check(drawn.Row("scale") && drawn.Row("scale")->detail.find(loc::T("settings.interface_size_detail")) != std::string::npos, "Interface size does not say that below 1.00x fits more");
}
}
// Each journey owns a Harness and with it the ImGui context, so they run one after another.
// Ember's links (platform::Links()): Help & About lists them in the table's order after the problem report, and
// each asks for its own link; Home's corner draws them as icons at the right of the brand's band where the window
// has the room. The list wraps as it always did with them drawn; they belong to the aside with the player's card,
// so Up from the card reaches them, Left and Right stay among them and Down returns to the card. A narrow window
// draws none, so neither the pad nor the pointer has a row the other lacks.
void EmberLinks() {
 namespace loc = sf4e::loc;
 using sf4e::platform::Links;
 using sf4e::platform::ServiceAction;
 const auto row = [](const sf4e::platform::Link& link) { return std::string("link-") + link.id; };
 const auto asked = [](const Harness& h, const char* id) {
  return std::count_if(h.actions.begin(), h.actions.end(), [&](const ShellAction& a) {
   return Is<commands::Service>(a, [&](const commands::Service& s) { return s.service == ServiceAction::OpenLink && s.link && std::string(s.link->id) == id; }); });
 };
 {
  Harness h; Drawn drawn; h.Screen("tab-about/about"); h.Frame(0, 3);
  const auto at = std::find_if(drawn.rows.begin(), drawn.rows.end(), [](const MenuEntry& e) { return e.id == "problem-report"; }) - drawn.rows.begin();
  bool ordered = at + static_cast<std::ptrdiff_t>(Links().size()) + 1 < static_cast<std::ptrdiff_t>(drawn.rows.size());
  for (std::size_t i = 0; ordered && i < Links().size(); ++i) {
   const MenuEntry& e = drawn.rows[static_cast<std::size_t>(at) + 1 + i];
   ordered = e.id == row(Links()[i]) && e.confirm && e.label == loc::T(Links()[i].label) && e.detail == loc::Tf(Links()[i].detail, Links()[i].address);
  }
  Check(ordered && drawn.rows[static_cast<std::size_t>(at) + 1 + Links().size()].id == "version", "Help & About does not list Ember's links in the table's order after the problem report");
  for (const auto& link : Links()) {
   const auto before = h.actions.size();
   h.FocusOn(row(link).c_str()); h.Press(MenuInput::Select);
   Check(h.actions.size() == before, "A link opened before it was confirmed");
   h.Press(MenuInput::Right); h.Press(MenuInput::Select);
   Check(h.actions.size() == before + 1 && asked(h, link.id) == 1, "A link in Help & About did not ask for its own link");
  }
  h.view.services.lastAction = ServiceAction::OpenLink; h.view.services.lastLink = sf4e::platform::FindLink("kofi"); h.view.services.message = "Opened in your browser. Switch to it.";
  h.Frame(0, 2);
  Check(drawn.Row("link-kofi")->detail == h.view.services.message && drawn.Row("link-twitter")->detail == loc::Tf("about.twitter_detail", "x.com/kabiefox"),
   "The service's message did not show on the one link it was about");
 }
 {
  Harness h; Drawn drawn; h.Screen("home"); h.Frame(0, 3);
  bool placed = true;
  for (std::size_t i = 0; i < Links().size(); ++i) {
   const auto id = row(Links()[i]);
   placed = placed && drawn.Shown(id.c_str()) && drawn.Row(id.c_str());
   if (i) placed = placed && drawn.boxes[id].first.x > drawn.boxes[row(Links()[i - 1])].second.x && drawn.Centre(id.c_str()).y == drawn.Centre(row(Links()[0]).c_str()).y;
  }
  Check(placed && drawn.Row("online") && drawn.boxes[row(Links().back())].second.y <= drawn.boxes["online"].first.y, "Home does not draw the links as a row of icons over the list");
  Check(drawn.boxes[row(Links().back())].second.x > ImGui::GetIO().DisplaySize.x * .6f, "Home's links are not at the right of the band");
  // The list wraps as it always did: Up from the top to the last row, Down from it to the top.
  h.shell.Navigation().Prefer("online"); h.Frame(0, 2);
  h.Press(MenuInput::Up);
  Check(h.shell.Navigation().Focus() == "news", "Up from Home's first row did not wrap to its last, with the links drawn");
  h.Press(MenuInput::Down);
  Check(h.shell.Navigation().Focus() == "online", "Down from Home's last row did not wrap to its first, with the links drawn");
  // The card and the links are one aside, moved by where they are drawn.
  h.shell.Navigation().Prefer("card"); h.Frame(0, 2);
  h.Press(MenuInput::Down);
  Check(h.shell.Navigation().Focus() == "card", "Down from the card left it");
  h.Press(MenuInput::Up);
  Check(h.shell.Navigation().Focus() == row(Links()[0]), "Up from the card did not reach the first link");
  h.Press(MenuInput::Up);
  Check(h.shell.Navigation().Focus() == row(Links()[0]), "Up from a link left the links");
  h.Press(MenuInput::Left);
  Check(h.shell.Navigation().Focus() == row(Links()[0]), "Left from the first link left the links");
  h.Press(MenuInput::Right); h.Press(MenuInput::Right);
  Check(h.shell.Navigation().Focus() == row(Links()[2]), "Right did not walk along the links");
  for (int i = 0; i < 5; ++i) h.Press(MenuInput::Right);
  Check(h.shell.Navigation().Focus() == row(Links().back()), "Right left the links at their end");
  h.Press(MenuInput::Down);
  Check(h.shell.Navigation().Focus() == "card", "Down from a link did not return to the card");
  const auto before = h.actions.size();
  h.FocusOn("link-kofi"); h.Press(MenuInput::Select); h.Press(MenuInput::Right); h.Press(MenuInput::Select);
  Check(h.actions.size() == before + 1 && asked(h, "kofi") == 1, "A link on Home's corner did not ask the question Help & About's row asks");
 }
 {
  Harness h; Drawn drawn; h.imgui.io.DisplaySize = ImVec2(700, 960); h.Screen("home"); h.Frame(0, 3);
  bool any = false;
  for (const auto& link : Links()) any = any || drawn.Row(row(link).c_str()) || drawn.boxes.count(row(link));
  Check(!any && drawn.Row("online"), "A narrow window drew or placed Home's corner links");
  for (int i = 0; i < 12; ++i) h.Press(MenuInput::Up);
  Check(h.shell.Navigation().Focus().compare(0, 5, "link-") != 0, "The pad reached a link the narrow window does not draw");
 }
}

void SettingsJourneys() {
 RoomSettingsFollowOneRule();
 RetrySaveOnPanelScreens();
 PanelFromRailFocusesHead();
 SaveWhileHidden();
 RowsFollowTheGame();
 MenuArtworkRow();
 ScreenModeRow();
 InterfaceSizeBelowOne();
 EmberLinks();
}
