#pragma once

// The game's replay files on this PC (ReplaySlots.hxx has their layout):
// copying every match replay out of the game's slots into Ember's archive,
// and putting an archived one back. Shared by the launcher, which archives
// around and during a game run, and Sidecar, which lists the archive and
// imports from it while the game runs.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
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
// active is the one of saves that belongs to the account signed in to the
// running Steam client (its ActiveUser), empty when Steam is not running or
// that account has no saves here.
struct Folders {
	std::vector<std::filesystem::path> saves;
	std::filesystem::path archive, active;
};
Folders FindFolders();

// Copies every match replay not yet in the archive, one file each, named by
// its save time (UTC) and CRC. The archive holds a replay when one of its
// files is that replay, whatever its save time: a replay put back into the
// game is saved there under a new time. A file counts by its contents, not
// its name. It is the slot's file that is read: one the game's index does not
// list yet is archived two minutes after it was written, with a record made
// from its own header. A slot caught while the game is writing it fails the
// size and CRC check and is copied on a later call. The archive is read
// through one index kept for this process, which opens a file once and again
// only when it changed. Returns how many were copied, or -1 when there is
// nowhere to copy from or to. The launcher's: it walks every account's slots
// and the whole archive folder.
int Archive();

// An archived replay put back into the game's files: the slot it took, the
// slot record as written (ReplaySlots.hxx: Import), the slot's two bytes and
// the writes that were made (ReplaySlots.hxx: WritePlan), which UndoImport
// takes back. The indexes are read from the signed-in account's folder
// (Folders::active), the one write stores into.
//
// Nothing is written unless the indexes match their checksum files, no match
// slot holds a replay the index has not caught up with (the game writes a
// match's file before its index, and the index would name that slot as the
// oldest), and the replay the chosen slot holds is in the archive, copied
// there now if need be. The files are then written in the plan's order, the
// replay before the indexes that name it; a write that fails puts back every
// file written so far.
//
// The game holds the slots in memory while it runs and writes them back on
// its next save, so a caller inside the game puts the record into its table
// too, only after this returned Done, and calls UndoImport when the table
// does not take it: the files would otherwise be ahead of the table, and the
// game's next save would write the old record over them.
//
// write stores each changed file: its name under the account's Steam Cloud
// folder (capcom/superstreetfighteriv/ssf4_savedata/<name>) and its bytes.
// The game reads its files through Steam, which keeps its own index of their
// sizes, so a file written beside it is read at its old size: inside the
// game, write goes through Steam's FileWrite.
//
// It reads the two indexes, the match slots' files (kept by size and time)
// and the one file to import; the archive folder is not walked.
struct Imported {
	int slot = -1;
	replayslots::Bytes record, slotBytes;
	replayslots::WritePlan plan;
};
// Why nothing was imported. IndexBehind and NotArchived pass by themselves
// once the game has saved; the others do not.
enum class ImportResult { Done, NoFolder, NotAReplay, IndexBehind, NotArchived, IndexDamaged, WriteFailed };
using Writer = std::function<bool(const std::string& name, const replayslots::Bytes& contents)>;
ImportResult ImportFile(const std::filesystem::path& file, const Writer& write, Imported& out);
// Puts the files of an import back as they were before it. False when one
// could not be.
bool UndoImport(const Imported& imported, const Writer& write);

// The game's record names no one for an Ember match, so Ember notes the
// two players itself when a match starts (matches.jsonl in the archive:
// the start time, both names, P1 first, and whether this PC only watched;
// the game records a spectated match too, and the two fighters). A replay is
// saved when the match ends, so it belongs to the last match started before
// its save time, within an hour, with its two fighters
// (ReplaySlots.hxx: MatchOf).
void NoteMatchStart(const std::string& p1, const std::string& p2, bool spectating, int fighter1, int fighter2);

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
	// An exported video (VideoOf) is beside it.
	bool video = false;
};
// The video an export of that replay writes: its name with ".mp4".
inline std::filesystem::path VideoOf(std::filesystem::path replay) { return replay.replace_extension(L".mp4"); }

// Lists the archive on a thread of its own: Ember's own files from the
// archive root, and usf4-replay-saver's (.usf4replay) from any folder under
// it, those with time and fighters from the replay's header and no names.
// Any thread may ask and read: WantListing asks for a listing, which starts
// within two seconds of the one before, and LatestListing is the last one
// made (null before the first). A listing opens only the files that are new
// or changed since the one before, so it does not grow with the archive, and
// nothing a file or folder does there reaches the caller. StopListing ends
// the thread and waits for a listing in progress; called once, at shutdown,
// and never under the loader lock.
void WantListing();
std::shared_ptr<const std::vector<ArchivedReplay>> LatestListing();
void StopListing();

} } }
