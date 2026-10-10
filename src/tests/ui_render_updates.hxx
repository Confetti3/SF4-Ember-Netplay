#pragma once
// The updater's channel picker and its two confirms, the going-back install
// row and What's new; Help and about's version and channel, and Home's
// version corner on Stable and on Nightly; in every render locale.
#include "ui_render_support.hxx"
#include "../common/Localization.hxx"
#include "../ui/ApplicationShell.hxx"
#include "../ui/RecoveryMenu.hxx"
#include <cstring>
#include <string>
#include <vector>
namespace {
// Release notes as GitHub sends them: CRLF, headings, lists, links, emphasis,
// an emoji the fonts cannot draw, and text that must stay text.
const char* const RenderReleaseNotes=
    "## Highlights\r\n\r\n"
    "- **Replays** open from [the archive](https://example.invalid/replays) with `Watch now` \xF0\x9F\x8E\x89\r\n"
    "- The frame meter shows on replays when *Watch with frame meter* is chosen\r\n"
    "  * Nested: export keeps its 100% volume and {0} captions\r\n\r\n"
    "### Fixes\r\n\r\n"
    "1. Rooms no longer freeze after a rematch on slow links.\r\n"
    "2. The updater offers a step back from Nightly to Stable.\r\n"
    "3. A controller can scroll long text such as these notes.\r\n\r\n"
    "> Players on Nightly: replays recorded there may not open on Stable.\r\n\r\n"
    "---\r\n"
    "<!-- internal: checklist -->\r\n"
    "Thanks to everyone who sent logs &amp; reports. Full changes: https://example.invalid/compare\r\n";
template<class Draw>
void ShootUpdates(sf4e::ui::GameMenu& menu,sf4e::platform::ServiceSnapshot& state,sf4e::ui::ChannelPick& pick,const Draw& draw) {
    using namespace sf4e;using namespace ui;
    state=platform::ServiceSnapshot{};state.installedVersion="1.1.0-rc1";state.channel=launcher::UpdateChannel::Beta;
    menu.navigation=RecoveryNavigation(true);menu.navigation.Prefer("channel");pick=ChannelPick{};draw();
    // Right picks Nightly without saving it; Select asks first, with Nightly's warning.
    draw(nullptr,MenuInput::Right,1);draw("update-channel-pick");
    Require(pick.Shown(state.channel)==launcher::UpdateChannel::Nightly&&state.channel==launcher::UpdateChannel::Beta,"Right did not pick Nightly, or saved it");
    draw(nullptr,MenuInput::Select,1);draw("update-channel-confirm-nightly");
    Require(menu.navigation.Confirming()&&!menu.navigation.ConfirmSelected(),"The channel confirm did not start on Cancel");
    draw(nullptr,MenuInput::Back,1);draw();
    // From Nightly, Left twice reaches Stable; its confirm says what going back means.
    state.channel=launcher::UpdateChannel::Nightly;state.installedVersion="1.3.0-nightly20261009.2";draw();
    draw(nullptr,MenuInput::Left,1);draw(nullptr,0,1);draw(nullptr,MenuInput::Left,1);draw();
    Require(pick.Shown(state.channel)==launcher::UpdateChannel::Stable,"Left did not pick Stable from Nightly");
    draw(nullptr,MenuInput::Select,1);draw("update-channel-confirm-back");
    Require(menu.navigation.Confirming(),"The going-back confirm did not open");
    draw(nullptr,MenuInput::Back,1);draw();pick=ChannelPick{};
    // Stable offers an older version than the Nightly installed: the install row says so.
    state.channel=launcher::UpdateChannel::Stable;state.update.ok=state.update.updateAvailable=state.update.goesBack=true;
    state.update.expectedSha256=std::string(64,'a');state.update.latestVersion="v1.1.2";state.update.releaseNotes=RenderReleaseNotes;
    menu.navigation=RecoveryNavigation(true);menu.navigation.Prefer("install");draw("update-going-back");
    menu.navigation.Prefer("notes");draw("update-whats-new");
    Require(menu.navigation.Focus()=="notes","What's new is missing from the updater");
    draw(nullptr,MenuInput::Select,1);draw("update-whats-new-reader");
    Require(menu.navigation.Reading(),"What's new did not open in the reader");
    // Held Down scrolls the notes wherever they are longer than the reader.
    draw(nullptr,MenuInput::Down,12);draw();
    const auto* reader=FindWindow("Menu reader/notes");
    Require(reader->ScrollMax.y<1||reader->Scroll.y>0,"Held Down did not scroll What's new");
    draw(nullptr,MenuInput::Back,1);draw();
    Require(!menu.navigation.Reading(),"Back did not close What's new");
    state=platform::ServiceSnapshot{};pick=ChannelPick{};menu.navigation=RecoveryNavigation(true);
}
template<class Draw,class Page>
void ShootVersionAndChannel(sf4e::ui::ApplicationShell& shell,sf4e::ui::ShellView& view,const Draw& draw,const Page& page) {
    using namespace sf4e;using namespace ui;
    // Home's corner names the version and channel on every channel.
    const std::string build=view.build;view.build="600479cd";
    // Drawn inside the window, and whole wherever there is room for it.
    std::string drawn;ImVec2 noteMin,noteMax;
    SetMenuCardProbe([&](const char* id,ImVec2 min,ImVec2 max){if(!std::strncmp(id,"footer-note",11)){drawn=id;noteMin=min;noteMax=max;}});
    const auto checkNote=[&](const char* shot){
        drawn.clear();draw(shot);
        const auto* home=FindWindow("EmberShell");
        // Only a window under 400 interface pixels tall (1280x720 at 200%)
        // has no line to spare under the legend; there it may be cut beside it.
        const bool cramped=ImGui::GetIO().DisplaySize.y/Scale()<400;
        Require(drawn=="footer-note"||(cramped&&drawn=="footer-note/cut"),"Home's version line is missing or cut");
        Require(noteMin.x>=home->Pos.x&&noteMax.x<=home->Pos.x+home->Size.x+1&&noteMax.y<=home->Pos.y+home->Size.y+1,"Home's version line left the window");
    };
    view.services.installedVersion="1.1.2";view.services.channel=launcher::UpdateChannel::Stable;
    shell.Navigation().Home();checkNote("home-stable");
    view.services.installedVersion="1.3.0-nightly20261009.2";view.services.channel=launcher::UpdateChannel::Nightly;
    checkNote("home-nightly");
    SetMenuCardProbe({});
    std::vector<MenuEntry> rows;SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries){rows=entries;});
    page("about");shell.Navigation().Focus("version",rows);draw("about-version");
    const auto row=[&](const char* id)->const MenuEntry*{for(const auto& e:rows)if(e.id==id)return &e;return nullptr;};
    Require(row("version")&&row("version")->value==VersionLine(view.services.installedVersion,view.services.channel)&&
        row("version")->detail.find("600479cd")!=std::string::npos,"Help and about does not show the version, channel and build");
    // Later shots open the controls from the screen's first row.
    shell.Navigation().Focus("help",rows);draw();SetMenuEntriesProbe({});
    view.services=platform::ServiceSnapshot{};view.build=build;shell.Navigation().Home();draw();
}
}
