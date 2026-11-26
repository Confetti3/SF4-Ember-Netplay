#pragma once
#include <vector>

namespace sf4e { namespace overlay {
// The quick lines a fighter asked for, by slot (QuickChat.hxx), from the pad's
// chord on the game thread and F1 to F4 on the window's thread, for the
// overlay's frame to send. At most four wait; a fifth drops the oldest. A line
// is for its moment: one that waited longer than StaleMs (Ember was shown, so
// no frame sent it) is dropped rather than sent late.
constexpr unsigned long long StaleMs = 2000;
// Whether quick lines can be sent now: a fight this PC fights in, with the
// player's quick lines on. The pad's update says it each frame (the game
// thread decides it, as it decides the chord); the window procedure reads it
// for F1 to F4.
void SetQuickLinesLive(bool live);
bool QuickLinesLive();
void PostQuickLine(int slot);
// Everything posted since the last call and not stale, oldest first.
std::vector<int> TakeQuickLines();
} }
