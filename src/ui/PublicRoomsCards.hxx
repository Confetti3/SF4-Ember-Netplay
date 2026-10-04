#pragma once
#include "MenuNavigation.hxx"
#include "PublicRoomsPanel.hxx"
#include "PublicRoomsSetup.hxx"
#include "../netplay/PublicRooms.hxx"
#include <imgui.h>
#include <string>

namespace sf4e { namespace ui {
struct ShellView;

// The corner radius of the public rooms' cards and toolbar cells, in unscaled pixels.
// GameMenu rounds its own background and focus mark to it (GameMenu::cardRounding).
constexpr float PublicCardRounding = 4;

// A room from a bridge that sends no details: nothing to show under its name and place,
// so its card is the short one with the two lines centred.
bool PublicRoomBare(const netplay::publicrooms::Room& room);

// GameMenu's Card hook for the Public rooms screen: paints the toolbar cells,
// the room cards (and the one being opened), and the state cards (checking,
// setup, loading, none, error) inside [min, max]. `focused`: the card the menu
// has selected, which gets the accent outline; `now` is the interface clock, for
// the sliding bars and the pulse. False for the rows that stay plain (Stop,
// the link row, Ember ID, Cancel).
// The workflow comes in explicitly, never read back from the entry: `setup` is
// the Ember ID panel's one-press setup (running, failed or neither), which the
// pr-setup card shows; `phase` is the panel's PhaseOf(entry.id), which swaps a
// room card's seat count for Joining or Connecting and runs its bar. The entry
// gives the text and whether its action can be pressed.
bool DrawPublicCard(const ShellView& view, const PublicRoomsPanel& panel, const PublicSetupView& setup, PublicRoomsPanel::CardPhase phase,
                    const MenuEntry& entry, ImVec2 min, ImVec2 max, bool focused, double now);

// GameMenu's Detail hook: under a room's name and sentence, the players' faces,
// its rules and how long it has been open. Draws at the cursor, inside the
// detail pane; nothing for any other row.
void DrawPublicPreview(const ShellView& view, const PublicRoomsPanel& panel, const std::string& id);

// A pill label at `at` (its top left): 20 tall, the text shrunk to fit
// `maxWidth` and reported to the overflow probe as `probe`. Returns its width,
// or 0 when there is no room to draw it.
float DrawChip(ImVec2 at, const char* text, ImU32 foreground, ImU32 background, const char* probe, float maxWidth);
} }
