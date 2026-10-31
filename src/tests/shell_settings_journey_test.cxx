// The settings' journeys that cross screens: the one rule for the settings a
// room leaves open, pad and pointer alike; Retry saving, the shell's own row,
// drawn and reached on every kind of screen; a Settings panel opened from the
// rail of another; and a save that goes on while Ember is hidden.
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
 return static_cast<std::size_t>(std::count_if(h.actions.begin() + static_cast<std::ptrdiff_t>(from), h.actions.end(), [](const ShellAction& a) { return a.roomSettings; }));
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
 h.view.preferences = h.actions.back().preferences; h.Frame();
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
  const bool saved = std::any_of(h.actions.begin() + static_cast<std::ptrdiff_t>(mark), h.actions.end(), [](const ShellAction& a) { return a.command.kind == Kind::SavePreferences; });
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
 Check(RoomSaves(h, mark) == 1 && h.actions.back().preferences.roomColumn == netplay::DefaultRoomColumn + netplay::RoomColumnStep,
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
 const auto sent = std::find_if(h.actions.begin() + static_cast<std::ptrdiff_t>(mark), h.actions.end(), [](const ShellAction& a) { return a.command.kind == Kind::SavePreferences; });
 Check(sent != h.actions.end() && sent->preferences.mainFighter == main, "A setting left unsaved was not saved while Ember was hidden");
 Check(h.shell.Navigation().Screen() == "main-character", "The main-character screen closed before the save was acknowledged");
 h.view.preferences = sent->preferences; hidden(1.f / 60);
 Check(h.shell.Navigation().Screen() == "profile", "The save's acknowledgment was not taken while Ember was hidden");
 const auto after = h.actions.size();
 h.Frame(0, 40);
 Check(std::none_of(h.actions.begin() + static_cast<std::ptrdiff_t>(after), h.actions.end(), [](const ShellAction& a) { return a.command.kind == Kind::SavePreferences; }),
  "The save went out again once Ember showed");
}
}
// Each journey owns a Harness and with it the ImGui context, so they run one after another.
void SettingsJourneys() {
 RoomSettingsFollowOneRule();
 RetrySaveOnPanelScreens();
 PanelFromRailFocusesHead();
 SaveWhileHidden();
}
