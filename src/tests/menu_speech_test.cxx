// What speech says as the player moves through a menu: a new screen's title
// and size, then only the row's name and value; the row's detail and button
// hint once, well after the player stops on it; a changed value alone; the
// row's place in the list only when wanted; and a dialog's question before
// its button.
#include "../ui/MenuSpeech.hxx"
#include "../common/Localization.hxx"
#include "test_support.hxx"
#include <string>
#include <vector>

using namespace sf4e;
using namespace sf4e::ui;

int main() {
    loc::SetActive(loc::Locale::En);
    std::vector<MenuEntry> rows{{"lag", "Lower input lag", "Reads the pad every frame.", "Off"}, {"hud", "Match HUD", "Shows the strip.", "On"}};
    rows[1].enabled = false;
    netplay::SpeechPreferences speech;
    CHECK(speech.details && !speech.hints && !speech.position);
    MenuNarrator narrator;
    SpokenFrame frame{"Player", &rows[0], 0, 2};
    frame.hint = "A: Select";
    auto said = narrator.Update(frame, 0, speech);
    CHECK(said.text == "Player, 2 items. Lower input lag, Off" && said.interrupt);
    // The detail waits well past the name, and comes once.
    CHECK(narrator.Update(frame, .5, speech).text.empty());
    CHECK(narrator.Update(frame, 1.4, speech).text.empty());
    said = narrator.Update(frame, 1.6, speech);
    CHECK(said.text == "Reads the pad every frame." && !said.interrupt);
    CHECK(narrator.Update(frame, 5, speech).text.empty());

    // Moving on interrupts with the name, value and state only.
    frame.focus = &rows[1]; frame.index = 1;
    said = narrator.Update(frame, 6, speech);
    CHECK(said.text == "Match HUD, On, Unavailable" && said.interrupt);
    // With hints wanted and details not, the pause reads the hint.
    speech.details = false; speech.hints = true;
    CHECK(narrator.Update(frame, 7.6, speech).text == "A: Select");

    // The place in the list, when wanted.
    speech.position = true; speech.hints = false;
    frame.focus = &rows[0]; frame.index = 0;
    CHECK(narrator.Update(frame, 8, speech).text == "Lower input lag, Off, 1 of 2");
    speech.position = false;

    // A changed value is said alone, and does not bring the detail back.
    speech.details = true;
    CHECK(narrator.Update(frame, 9.6, speech).text == "Reads the pad every frame.");
    rows[0].value = "On";
    CHECK(narrator.Update(frame, 10, speech).text == "On");
    CHECK(narrator.Update(frame, 12, speech).text.empty());

    // A dialog says its question, then each button the player moves to; once
    // it closes, the row is said again.
    frame.question = "Lower input lag"; frame.button = "Cancel";
    CHECK(narrator.Update(frame, 13, speech).text == "Lower input lag. Cancel");
    frame.button = "Turn on";
    CHECK(narrator.Update(frame, 14, speech).text == "Turn on");
    frame.question.clear(); frame.button.clear();
    CHECK(narrator.Update(frame, 15, speech).text == "Lower input lag, On");

    // A new screen says its title and size before its row.
    frame.title = "Interface";
    CHECK(narrator.Update(frame, 20, speech).text == "Interface, 2 items. Lower input lag, On");
    return 0;
}
