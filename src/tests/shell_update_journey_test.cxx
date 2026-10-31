#include "shell_journey_support.hxx"
#include "../launcher/update/InstallOffer.hxx"
// The launch-time update check from the player's side: the check is asked for
// on the first Home, the question comes only where it can be answered, its
// three buttons send what they say, and the Settings row saves on its own.
namespace {
using sf4e::launcher::InstallOffer;
using sf4e::platform::LaunchCheck;
using sf4e::platform::ServiceAction;
const InstallOffer Offer{sf4e::launcher::UpdateChannel::Nightly, "v1.3.0-nightly20261014", std::string(64, 'c')};
std::size_t Count(const Harness& h, ServiceAction action) {
 return static_cast<std::size_t>(std::count_if(h.actions.begin(), h.actions.end(), [&](const ShellAction& a) { return a.service == action; }));
}
// A service that found `Offer`.
void Found(Harness& h) { h.view.services.launchCheck = LaunchCheck::Settled; h.view.services.launchOffer = Offer; }
}
void UpdateLaunchJourneys() {
 using namespace sf4e;
 // A service that makes no launch check is never asked for one.
 {
  Harness h; h.Screen("home"); h.Frame(0, 120);
  Check(Count(h, ServiceAction::LaunchUpdateCheck) == 0 && !h.shell.NoticeOpen(), "A view with no launch check asked for one");
 }
 // The first Home asks once; a service that has taken the request is not asked again.
 {
  Harness h; h.view.services.launchCheck = LaunchCheck::Waiting;
  h.Screen("tab-player"); h.Frame(0, 30);
  Check(Count(h, ServiceAction::LaunchUpdateCheck) == 0, "The launch check was asked for away from Home");
  h.Screen("home"); h.Frame(0, 10);
  Check(Count(h, ServiceAction::LaunchUpdateCheck) == 1, "The first Home did not ask for the launch check once");
  h.view.services.launchCheck = LaunchCheck::Running; h.Frame(0, 180);
  Check(Count(h, ServiceAction::LaunchUpdateCheck) == 1, "A running launch check was asked for again");
 }
 // Update now sends the offer that was shown, and the Launcher is the service's to open.
 {
  Harness h; Found(h); h.Screen("home"); h.Frame(0, 2);
  Check(h.shell.NoticeOpen(), "Home did not ask about the update it was offered");
  const auto before = h.actions.size();
  h.Press(MenuInput::Left); h.Press(MenuInput::Select);
  Check(!h.shell.NoticeOpen() && h.actions.size() == before + 1 && h.actions.back().service == ServiceAction::UpdateNow &&
   h.actions.back().updateOffer == Offer, "Update now did not send the offer that was shown");
  h.Frame(0, 30);
  Check(!h.shell.NoticeOpen(), "The question came back after it was answered");
 }
 // Skip this version sends the offer, and nothing else.
 {
  Harness h; Found(h); h.Screen("home"); h.Frame(0, 2);
  const auto before = h.actions.size();
  h.Press(MenuInput::Right); h.Press(MenuInput::Select);
  Check(!h.shell.NoticeOpen() && h.actions.size() == before + 1 && h.actions.back().service == ServiceAction::SkipUpdate &&
   h.actions.back().updateOffer == Offer, "Skip this version did not send the offer that was shown");
 }
 // A skip the queue refused is said on Home, and the question comes back so it can be answered again.
 {
  Harness h; Found(h); h.Screen("home"); h.Frame(0, 2);
  std::string status; Tone tone = Tone::Neutral;
  SetMenuStatusProbe([&](const char* text, Tone t) { status = text; tone = t; });
  h.accept = false;
  h.Press(MenuInput::Right); h.Press(MenuInput::Select);
  Check(Count(h, ServiceAction::SkipUpdate) == 1 && status == loc::T("error.queue_failed") && tone == Tone::Error, "A refused skip went unsaid");
  h.Frame(0, 3);
  Check(h.shell.NoticeOpen(), "A skip the queue refused was not offered again");
  h.accept = true;
  h.Press(MenuInput::Right); h.Press(MenuInput::Select);
  Check(Count(h, ServiceAction::SkipUpdate) == 2 && !h.shell.NoticeOpen(), "The skip could not be sent again");
  SetMenuStatusProbe({});
 }
 // A skip the service could not save is said on Home and offered again; a saved one is final.
 {
  Harness h; Found(h); h.Screen("home"); h.Frame(0, 2);
  std::string status; Tone tone = Tone::Neutral;
  SetMenuStatusProbe([&](const char* text, Tone t) { status = text; tone = t; });
  h.Press(MenuInput::Right); h.Press(MenuInput::Select);
  Check(Count(h, ServiceAction::SkipUpdate) == 1, "The skip was not sent");
  h.Frame(0, 120);
  Check(!h.shell.NoticeOpen() && status != loc::T("common.save_failed"), "The question came back before the service answered the skip");
  h.view.services.skipFailures = 1; h.view.services.skipError = loc::T("common.save_failed");
  h.Frame(0, 3);
  Check(status == loc::T("common.save_failed") && tone == Tone::Error, "A skip that was not saved went unsaid");
  Check(h.shell.NoticeOpen(), "A skip that was not saved was not offered again");
  h.Press(MenuInput::Right); h.Press(MenuInput::Select);
  Check(Count(h, ServiceAction::SkipUpdate) == 2 && !h.shell.NoticeOpen(), "The unsaved skip could not be sent again");
  h.Frame(0, 120);
  Check(!h.shell.NoticeOpen(), "The question came back while the second skip was unanswered");
  h.view.services.launchOffer.reset(); h.Frame(0, 120);
  Check(!h.shell.NoticeOpen() && Count(h, ServiceAction::SkipUpdate) == 2, "A saved skip came back");
  SetMenuStatusProbe({});
 }
 // Later is highlighted, so a press meant for the menu behind cannot close the game; it and Back send nothing
 // and the question is not asked again in the same run.
 {
  Harness h; Found(h); h.Screen("home"); h.Frame(0, 2);
  const auto before = h.actions.size();
  h.Press(MenuInput::Select);
  Check(!h.shell.NoticeOpen() && h.actions.size() == before, "Later sent something");
  h.Frame(0, 120);
  Check(!h.shell.NoticeOpen() && h.actions.size() == before, "Later was asked again in the same run");
 }
 {
  Harness back; Found(back); back.Screen("home"); back.Frame(0, 2);
  const auto backBefore = back.actions.size();
  back.Press(MenuInput::Back);
  Check(!back.shell.NoticeOpen() && back.actions.size() == backBefore, "Back on the question sent something");
 }
 // An Update now whose Launcher would not start is said, once, where the player is.
 {
  Harness h; h.Screen("home");
  std::string status; Tone tone = Tone::Neutral;
  SetMenuStatusProbe([&](const char* text, Tone t) { status = text; tone = t; });
  h.view.services.lastAction = ServiceAction::UpdateNow; h.view.services.succeeded = false; h.view.services.message = "The updater could not be started.";
  h.Frame(0, 3);
  Check(status == h.view.services.message && tone == Tone::Error, "A failed Update now went unsaid");
  h.Frame(0, 400);
  Check(status != h.view.services.message, "A failed Update now was said again and again");
  SetMenuStatusProbe({});
 }
 // The question has three buttons, named by the catalog, with Later highlighted.
 {
  const auto dialog = UpdatePrompt::Dialog(Offer, "v1.2.0-nightly20261012");
  Check(dialog.buttons.size() == 3 && dialog.buttons[0].label == loc::T("updates.launch_now") && dialog.buttons[0].result == NoticeResult::UpdateNow &&
   dialog.buttons[1].label == loc::T("updates.launch_later") && dialog.buttons[1].result == NoticeResult::Acknowledged &&
   dialog.buttons[2].label == loc::T("updates.launch_skip") && dialog.buttons[2].result == NoticeResult::SkipUpdate && dialog.selected == 1,
   "The question's buttons are not Update now, Later and Skip this version");
  Check(dialog.text.find("v1.3.0-nightly20261014") != std::string::npos && dialog.text.find("v1.2.0-nightly20261012") != std::string::npos,
   "The question does not name the offered and the installed versions");
 }
 // Only where the player can answer: never in a room, a match, Training or another screen.
 {
  Harness h; Found(h); JoinedRoom(h, 4); h.view.canEditPreferences = false; h.Frame(0, 3);
  h.shell.Navigation().Home(); h.Frame(0, 120);
  Check(h.shell.Navigation().Screen() == "home" && !h.shell.NoticeOpen(), "The question came up in a room");
  h.view.session.match = netplay::MatchState::Playing; h.Frame(0, 30);
  Check(!h.shell.NoticeOpen(), "The question came up in a match");
  h.view.session.room = netplay::RoomState::Idle; h.view.room = {}; h.view.session.match = netplay::MatchState::None;
  h.view.atMainMenu = false; h.view.canEditPreferences = false; h.Frame(0, 30);
  Check(!h.shell.NoticeOpen(), "The question came up away from the main menu (Training)");
  h.view.practiceFight = true; h.view.atMainMenu = true; h.view.canEditPreferences = true; h.Frame(0, 30);
  Check(!h.shell.NoticeOpen(), "The question came up over a Practice match");
  h.view.practiceFight = false; h.view.watchedGame = {0, 1}; h.Frame(0, 30);
  Check(!h.shell.NoticeOpen(), "The question came up over a watched game");
  h.view.watchedGame = {}; h.view.inputCapture = input::Capture::Press; h.Frame(0, 30);
  Check(!h.shell.NoticeOpen(), "The question came up while a button was being assigned");
  h.view.inputCapture = input::Capture::Idle; h.Screen("tab-player"); h.Frame(0, 30);
  Check(!h.shell.NoticeOpen(), "The question came up on a screen that is not Home");
  h.Screen("home"); h.Frame(0, 3);
  Check(h.shell.NoticeOpen(), "The question did not come up once the player was free on Home");
 }
 // After the launch-time settings notice, and never over another dialog.
 {
  Harness h; Found(h);
  h.view.showGameSettingsCard = true; h.view.gameSettings.frameRate = "VARIABLE";
  h.Screen("home"); h.Frame(0, 3);
  Check(h.shell.NoticeOpen(), "The settings notice did not open");
  const auto before = h.actions.size();
  h.Press(MenuInput::Select);
  Check(h.actions.size() == before, "Dismissing the settings notice sent something");
  h.Frame(0, 3);
  Check(h.shell.NoticeOpen(), "The question did not follow the settings notice");
  h.Press(MenuInput::Left); h.Press(MenuInput::Select);
  Check(h.actions.back().service == ServiceAction::UpdateNow, "The answer after the settings notice was lost");
 }
 {
  Harness other; Found(other);
  other.view.readyFailure = "The ready press failed."; other.view.readyFailureSequence = 1;
  other.Screen("home"); other.Frame(0, 3);
  Check(other.shell.NoticeOpen() && other.shell.NoticeError(), "The error did not open");
  other.Frame(0, 60);
  Check(other.shell.NoticeError(), "The question took the place of an error");
  other.Press(MenuInput::Select); other.Frame(0, 3);
  Check(other.shell.NoticeOpen() && !other.shell.NoticeError(), "The question did not follow the error");
 }
 // The Settings row: on by default, saved on its own, and a failed save is said.
 {
  Harness h; std::vector<bool> saved;
  h.shell.SetUpdateCheckSaver([&](bool enabled, std::string&) { saved.push_back(enabled); return true; });
  std::vector<MenuEntry> rows;
  SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries) { rows = entries; });
  const auto row = [&]() -> const MenuEntry& {
   auto found = std::find_if(rows.begin(), rows.end(), [](const MenuEntry& e) { return e.id == "launch-update-check"; });
   Check(found != rows.end(), "The Check for updates at launch row is missing"); return *found;
  };
  h.Screen("about"); h.FocusOn("launch-update-check");
  Check(row().label == "Check for updates at launch" && row().value == loc::T("common.on") && row().adjustable && row().enabled, "The row is not an enabled On value");
  h.Press(MenuInput::Left);
  Check(saved == std::vector<bool>{false} && row().value == loc::T("common.off"), "Turning the row off did not save Off");
  h.Press(MenuInput::Right);
  Check(saved == (std::vector<bool>{false, true}) && row().value == loc::T("common.on"), "Turning the row on did not save On");
  // Nothing for the runtime to do: no service, and no save of the room preferences.
  Check(std::none_of(h.actions.begin(), h.actions.end(), [](const ShellAction& a) {
   return a.service != ServiceAction::None || a.command.kind == netplay::CommandKind::SavePreferences; }), "The row sent a command to the runtime");
  SetMenuEntriesProbe({});
 }
 {
  Harness failing; failing.shell.SetUpdateCheckSaver([](bool, std::string& diagnostic) { diagnostic = "Read-only folder"; return false; });
  failing.view.checkUpdatesAtLaunch = true;
  std::vector<MenuEntry> rows;
  SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries) { rows = entries; });
  const auto row = [&]() -> const MenuEntry& {
   auto found = std::find_if(rows.begin(), rows.end(), [](const MenuEntry& e) { return e.id == "launch-update-check"; });
   Check(found != rows.end(), "The Check for updates at launch row is missing"); return *found;
  };
  failing.Screen("about"); failing.FocusOn("launch-update-check"); failing.Press(MenuInput::Left);
  Check(row().value == loc::T("common.on") && row().detail == loc::T("common.save_failed"), "A failed save changed the row or went unsaid");
  SetMenuEntriesProbe({});
 }
 // The channel row: Right picks without sending, Select asks first starting on
 // Cancel, and only a confirmed switch is sent with the channel that was picked.
 {
  Harness h; h.view.services.channel = launcher::UpdateChannel::Stable;
  std::vector<MenuEntry> rows;
  SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries) { rows = entries; });
  const auto row = [&]() -> const MenuEntry& {
   auto found = std::find_if(rows.begin(), rows.end(), [](const MenuEntry& e) { return e.id == "channel"; });
   Check(found != rows.end(), "Help & about has no update channel row"); return *found;
  };
  h.Screen("about"); h.FocusOn("channel");
  Check(row().value == loc::T("updates.channel.stable") && row().adjustable && !row().confirm, "The channel row does not show the saved Stable");
  const auto before = h.actions.size();
  h.Press(MenuInput::Right);
  Check(h.actions.size() == before && row().value == loc::T("updates.channel.prerelease") && row().confirm, "Right did not pick Beta without sending it");
  h.Press(MenuInput::Select);
  Check(h.shell.Navigation().Confirming() && !h.shell.Navigation().ConfirmSelected(), "The switch did not ask first, on Cancel");
  h.Press(MenuInput::Select);
  Check(h.actions.size() == before, "Cancel switched the channel");
  h.Press(MenuInput::Select); h.Press(MenuInput::Right); h.Press(MenuInput::Select);
  Check(h.actions.size() == before + 1 && h.actions.back().service == ServiceAction::SwitchUpdateChannel &&
   h.actions.back().updateChannel == launcher::UpdateChannel::Beta, "Confirming did not send the switch to Beta");
  // Once the service has saved it, the row shows the saved channel.
  h.view.services.channel = launcher::UpdateChannel::Beta; h.Frame(0, 2);
  Check(row().value == loc::T("updates.channel.prerelease") && !row().confirm, "The saved channel did not settle the row");
  SetMenuEntriesProbe({});
 }
 // A check in Help & about that found a release, here a step back after a
 // channel switch, offers Update now, which sends exactly what was found.
 {
  Harness h; h.view.services.offer = InstallOffer{launcher::UpdateChannel::Stable, "v1.3.0", std::string(64, 'a')};
  std::vector<MenuEntry> rows;
  SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries) { rows = entries; });
  h.Screen("about"); h.FocusOn("update-now");
  const auto row = std::find_if(rows.begin(), rows.end(), [](const MenuEntry& e) { return e.id == "update-now"; });
  Check(row != rows.end() && row->label == loc::T("updates.launch_now") && row->confirm && row->enabled, "Help & about does not offer Update now for what it found");
  const auto before = h.actions.size();
  h.Press(MenuInput::Select); h.Press(MenuInput::Right); h.Press(MenuInput::Select);
  Check(h.actions.size() == before + 1 && h.actions.back().service == ServiceAction::UpdateNow &&
   h.actions.back().updateOffer == InstallOffer{launcher::UpdateChannel::Stable, "v1.3.0", std::string(64, 'a')}, "Update now did not send the release that was found");
  SetMenuEntriesProbe({});
 }
 {
  // A view that holds it off shows Off.
  Harness off; off.view.checkUpdatesAtLaunch = false;
  std::vector<MenuEntry> rows;
  SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries) { rows = entries; });
  off.Screen("about"); off.Frame(0, 2);
  const auto found = std::find_if(rows.begin(), rows.end(), [](const MenuEntry& e) { return e.id == "launch-update-check"; });
  Check(found != rows.end() && found->value == loc::T("common.off"), "The row did not show the saved Off");
  SetMenuEntriesProbe({});
 }
}
