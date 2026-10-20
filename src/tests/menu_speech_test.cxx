// What speech says as the player moves through a menu, the way Tokon reads
// its menus: a new screen's title, then a row's name, with its value only
// where the value is the point; the description only when asked for (F2) or
// wanted after a stop; a changed value alone; the place in the list only when
// wanted; a dialog's question before its button, and a notice that opened by
// itself kept quiet; each part apart, and a new screen's rows to prepare.
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
    CHECK(!speech.details && !speech.hints && !speech.position);
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

    // A notice that opened by itself is quiet until the player moves in it or asks.
    frame.notice = true; frame.question = "A new version is ready."; frame.button = "Later";
    CHECK(narrator.Update(frame, 27, speech).parts.empty());
    frame.button = "Update now";
    CHECK(narrator.Update(frame, 28, speech).Text() == "Update now");
    CHECK(narrator.Update(frame, 29, speech, true).Text() == "A new version is ready.");
    frame.notice = false; frame.question.clear(); frame.button.clear();
    narrator.Update(frame, 30, speech);

    // A new screen says its title before its row.
    frame.title = "Interface";
    CHECK(narrator.Update(frame, 31, speech).Text() == "Interface, Lower input lag, On");
    return 0;
}
