#pragma once
// A request to leave the main menu for Training mode. The shell makes it, the
// runtime judges it against the live session when it takes it off its queue,
// and only an accepted one asks the native menu to move (sf4e__GameEvents).
// Room: from a joined room, which stays joined; no room command is sent.
// Offline Training is the game's own Play offline > Training.
namespace sf4e {
enum class TrainingEntry { None, Room };
}
