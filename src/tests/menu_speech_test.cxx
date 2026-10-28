// What speech says as the player moves through a menu, the way Tokon reads
// its menus: a new screen's title, then a row's name, with its value only
// where the value is the point; the description only when asked for (F2) or
// wanted after a stop; a changed value alone; the place in the list only when
// wanted; a dialog's question before its button, and a notice that opened by
// itself announced with a chime and read whole on the accessibility button;
// each part apart, and a new screen's rows to prepare.
#include "../ui/MenuSpeech.hxx"
#include "../common/Localization.hxx"
#include "test_support.hxx"
#include <string>
#include <vector>

using namespace sf4e;
using namespace sf4e::ui;

int main() {
    loc::SetActive(loc::Locale::En);
    std::vector<MenuEntry> rows{{"lag", "Lower input lag", "Reads the pad every frame.", "Off"}, {"hud", "Match HUD", "Shows the strip.", "On"},
        {"version", "Installed version", "The version this PC runs.", "1.4.0"}};
    rows[0].adjustable = rows[1].adjustable = true;
    rows[1].enabled = false;
    netplay::SpeechPreferences speech;
    CHECK(!speech.details && !speech.hints && !speech.position && speech.Reminds() && speech.tick);
    // The reminders have their own checks at the end.
    speech.remind = netplay::SpeechPreferences::Never;
    MenuNarrator narrator;
    SpokenFrame frame{"Player", &rows[0], 0, 3};
    frame.hint = "A: Select";
    frame.entries = &rows;
    auto said = narrator.Update(frame, 0, speech);
    CHECK(said.parts == std::vector<std::string>({"Player", "Lower input lag", "Off"}) && said.interrupt);
    CHECK(said.render == std::vector<std::string>({"Lower input lag", "Off", "Match HUD", "On", "Installed version"}));
    CHECK(narrator.Update(frame, .1, speech).render.empty());
    // No description by itself; F2 asks for it.
    CHECK(narrator.Update(frame, 5, speech).parts.empty());
    said = narrator.Update(frame, 6, speech, true);
    CHECK(said.Text() == "Reads the pad every frame." && said.interrupt && !said.stop);
    // A second press on the same row stops it; after a move it describes again.
    said = narrator.Update(frame, 6.5, speech, true);
    CHECK(said.stop && said.parts.empty());
    CHECK(narrator.Update(frame, 6.6, speech, true).Text() == "Reads the pad every frame.");
    narrator.Update(frame, 6.7, speech, true);

    // A move says the name; the value only where the player changes it; no state words.
    frame.focus = &rows[1]; frame.index = 1;
    CHECK(narrator.Update(frame, 7, speech).Text() == "Match HUD, On");
    frame.focus = &rows[2]; frame.index = 2;
    CHECK(narrator.Update(frame, 8, speech).Text() == "Installed version");

    // Descriptions by themselves, once, after the stop; hints with them when wanted.
    speech.details = speech.hints = true;
    frame.focus = &rows[0]; frame.index = 0;
    narrator.Update(frame, 9, speech);
    CHECK(narrator.Update(frame, 10, speech).parts.empty());
    said = narrator.Update(frame, 10.6, speech);
    CHECK(said.Text() == "Reads the pad every frame. A: Select" && !said.interrupt);
    CHECK(narrator.Update(frame, 20, speech).parts.empty());
    speech.details = speech.hints = false;

    // The place in the list, when wanted.
    speech.position = true;
    frame.focus = &rows[1]; frame.index = 1;
    CHECK(narrator.Update(frame, 21, speech).Text() == "Match HUD, On, 2 of 3");
    speech.position = false;

    // A changed value is said alone.
    frame.focus = &rows[0]; frame.index = 0;
    narrator.Update(frame, 22, speech);
    rows[0].value = "On";
    CHECK(narrator.Update(frame, 23, speech).Text() == "On");

    // A dialog the player opens says its question, then each button; once it
    // closes, the row is said again.
    frame.question = "Lower input lag"; frame.button = "Cancel";
    CHECK(narrator.Update(frame, 24, speech).Text() == "Lower input lag, Cancel");
    frame.button = "Turn on";
    CHECK(narrator.Update(frame, 25, speech).Text() == "Turn on");
    frame.question.clear(); frame.button.clear();
    CHECK(narrator.Update(frame, 26, speech).Text() == "Lower input lag, On");

    // A notice that opened by itself chimes and says how to hear it; the
    // accessibility button reads its text and buttons; moving says the button.
    frame.notice = true; frame.question = "A new version is ready."; frame.button = "Later";
    frame.buttons = {"Later", "Update now"}; frame.accessKey = "RT";
    said = narrator.Update(frame, 27, speech);
    CHECK(said.chime && said.Text() == "Notice. Press RT to hear it." && said.interrupt);
    CHECK(narrator.Update(frame, 27.5, speech, true).Text() == "A new version is ready. Later, Update now");
    frame.button = "Update now";
    said = narrator.Update(frame, 28, speech);
    CHECK(said.Text() == "Update now" && !said.chime);
    CHECK(narrator.Update(frame, 29, speech, true).Text() == "A new version is ready. Later, Update now");
    frame.notice = false; frame.question.clear(); frame.button.clear();
    narrator.Update(frame, 30, speech);

    // A new screen says its title before its row.
    frame.title = "Interface";
    said = narrator.Update(frame, 31, speech);
    CHECK(said.Text() == "Interface, Lower input lag, On");
    // Past the reminders, a row with a description ticks; one without does not.
    CHECK(said.tick);
    frame.focus = &rows[2]; rows[2].detail.clear();
    CHECK(!narrator.Update(frame, 32, speech).tick);

    // Until the player knows the accessibility button, a row with a
    // description names it, and a screen's first visit says it gives help.
    rows[2].detail = "The version this PC runs.";
    speech = {};
    MenuNarrator learning;
    frame.title = "Player"; frame.focus = &rows[0]; frame.index = 0;
    CHECK(learning.Update(frame, 40, speech).Text() == "Player, RT for help on any item, Lower input lag, On, RT for more");
    frame.focus = &rows[2];
    said = learning.Update(frame, 41, speech);
    CHECK(said.Text() == "Installed version, RT for more" && !said.tick);
    frame.title = "Interface"; learning.Update(frame, 42, speech);
    frame.title = "Player";
    CHECK(learning.Update(frame, 43, speech).Text() == "Player, Installed version, RT for more");
    // Three descriptions later it is a tick; Always keeps the words, and Never drops the help too.
    speech.describeUsed = 3;
    frame.focus = &rows[0];
    said = learning.Update(frame, 44, speech);
    CHECK(said.Text() == "Lower input lag, On" && said.tick);
    speech.remind = netplay::SpeechPreferences::Always;
    frame.focus = &rows[2];
    CHECK(learning.Update(frame, 45, speech).Text() == "Installed version, RT for more");
    return 0;
}
