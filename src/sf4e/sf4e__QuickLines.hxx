#pragma once

namespace sf4e { namespace overlay {
// The quick lines a fighter asked for, as one bit per slot (QuickChat.hxx; bit n
// is slot n), from the pad's chord on the game thread and F1 to F4 on the
// window's thread, for the overlay's frame to send. A line is for its moment:
// the frame takes what was asked since the last one, and drops it while Ember is
// shown over the game rather than sending it late.
// Whether quick lines can be sent now: a fight this PC fights in, with the
// player's quick lines on. The pad's update decides it each frame, on the game
// thread, as it decides the chord; the window procedure reads that decision for
// F1 to F4, and the shell applies only the room's rules.
void SetQuickLinesLive(bool live);
bool QuickLinesLive();
void PostQuickLine(int slot);
// Everything posted since the last call, and none of it again.
unsigned TakeQuickLines();
} }
