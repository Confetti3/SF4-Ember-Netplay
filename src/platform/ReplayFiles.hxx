#pragma once

// The game's replay files on this PC (ReplaySlots.hxx has their layout):
// copying every match replay out of the game's slots into Ember's archive,
// and putting an archived one back. Shared by the launcher, which does both
// around a game run, and Sidecar, which imports while the game runs.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "../common/ReplaySlots.hxx"

namespace sf4e { namespace platform { namespace replays {

// The slots a Versus battle saves into: the match list Sidecar gives the game
// (sf4e__Game__Battle.cxx). The stock game uses 300 to 309 and keeps replays
// the native online service handed out in 280 to 299, which are copied too.
constexpr int kFirstMatchSlot = 280, kLastMatchSlot = replayslots::kSlots - 1;

// The game's save folders, one for each Steam account that has played on this
// PC, and the folder the replays are copied to (%APPDATA%\sf4e\replays).
// Both empty when Windows has no Steam path or settings folder for this user.
struct Folders {
	std::vector<std::filesystem::path> saves;
	std::filesystem::path archive;
};
Folders FindFolders();

// Copies every match replay not yet in the archive, one file each, named by
// its save time (UTC) and CRC. A slot caught while the game is writing it
// fails the size and CRC check and is copied on a later call. Returns how
// many were copied, or -1 when there is nowhere to copy from or to.
int Archive();

// An archived replay put back into the game's files: the slot it took, the
// slot record as written (ReplaySlots.hxx: Import) and the slot's two bytes.
// The slots are archived first, so the replay the slot held is kept. The
// game holds the slots in memory while it runs and writes them back on its
// next save, so a caller inside the game puts the record into its table too.
// write stores each changed file: its name under the account's Steam Cloud
// folder (capcom/superstreetfighteriv/ssf4_savedata/<name>) and its bytes.
// The game reads its files through Steam, which keeps its own index of their
// sizes, so a file written beside it is read at its old size: inside the
// game, write goes through Steam's FileWrite.
struct Imported {
	int slot = -1;
	replayslots::Bytes record, slotBytes, replay;
};
using Writer = std::function<bool(const std::string& name, const replayslots::Bytes& contents)>;
bool ImportFile(const std::filesystem::path& file, const Writer& write, Imported& out);

// The game's record names no one for an Ember match, so Ember notes the
// two players itself when a match starts (matches.jsonl in the archive:
// the start time, both names, P1 first, and whether this PC only watched;
// the game records a spectated match too). A replay is saved when the
// match ends, so it belongs to the last match started before its save
// time, within an hour.
void NoteMatchStart(const std::string& p1, const std::string& p2, bool spectating);

// Remembers that an archived replay was put into the game (watched.txt in
// the archive, one file name per line), so the list can say so.
void MarkWatched(const std::filesystem::path& file);

// The archive, newest first. label is the save time in local time, fighters
// the two native fighter IDs from the replay's record (-1 when the file
// holds none), names the players Ember noted (empty when it noted none),
// spectated whether this PC only watched that match, and watched whether
// it was put into the game before.
struct ArchivedReplay {
	std::filesystem::path path;
	std::string label;
	std::uint64_t time = 0;
	int fighters[2] = {-1, -1};
	std::string names[2];
	bool spectated = false, watched = false;
};
std::vector<ArchivedReplay> ListArchive();

} } }
