// What speech says as the player moves through a menu: the screen and the
// row at once, the row's detail and button hint only once the player stays
// on it, a changed value alone, and a dialog's question before its button.
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
    MenuNarrator narrator;
    SpokenFrame frame{"Player", &rows[0], 0, 2};
    frame.hint = "A: Select";
    auto said = narrator.Update(frame, 0, speech);
    CHECK(said.text == "Player. Lower input lag, Off, 1 of 2" && said.interrupt);
    // Nothing more until the player has stayed on the row.
    CHECK(narrator.Update(frame, .2, speech).text.empty());
    said = narrator.Update(frame, .5, speech);
    CHECK(said.text == "Reads the pad every frame. A: Select" && !said.interrupt);
    CHECK(narrator.Update(frame, 2, speech).text.empty());

    // Moving on interrupts, says the row's state, and starts the wait again.
    frame.focus = &rows[1]; frame.index = 1;
    said = narrator.Update(frame, 3, speech);
    CHECK(said.text == "Match HUD, On, Unavailable, 2 of 2" && said.interrupt);
    speech.details = false;
    CHECK(narrator.Update(frame, 3.5, speech).text == "A: Select");
    speech.hints = false;
    frame.focus = &rows[0]; frame.index = 0;
    narrator.Update(frame, 4, speech);
    CHECK(narrator.Update(frame, 5, speech).text.empty());

    // A changed value is said alone.
    rows[0].value = "On";
    CHECK(narrator.Update(frame, 6, speech).text == "On");

    // A dialog says its question, then each button the player moves to; once
    // it closes, the row is said again.
    frame.question = "Lower input lag"; frame.button = "Cancel";
    CHECK(narrator.Update(frame, 7, speech).text == "Lower input lag. Cancel");
    frame.button = "Turn on";
    CHECK(narrator.Update(frame, 8, speech).text == "Turn on");
    frame.question.clear(); frame.button.clear();
    CHECK(narrator.Update(frame, 9, speech).text == "Lower input lag, On, 1 of 2");

    // A new screen says its title before its row.
    frame.title = "Interface";
    CHECK(narrator.Update(frame, 10, speech).text == "Interface. Lower input lag, On, 1 of 2");
    return 0;
}
