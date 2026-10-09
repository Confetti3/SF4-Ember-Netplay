#pragma once
// A request to leave the main menu for Training mode. The shell makes it, the
// runtime judges it against the live session when it takes it off its queue,
// and only an accepted one asks the native menu to move (sf4e__GameEvents).
// Room: from a joined room, which stays joined; no room command is sent.
// Offline: with a StartOffline, once the runtime has carried that out.
namespace sf4e {
enum class TrainingEntry { None, Room, Offline };
}
