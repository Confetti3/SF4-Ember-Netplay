#pragma once

// The game's replay files on this PC (ReplaySlots.hxx has their layout):
// copying every match replay out of the game's slots into Ember's archive,
// and putting an archived one back. Shared by the launcher, which archives
// before and after a game run, and Sidecar, which archives as the game saves,
// lists the archive and imports from it while the game runs.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../common/ReplayInputDetails.hxx"
#include "../common/ReplaySlots.hxx"
#include "../common/ReplayFileSafety.hxx"
#include "../common/ReplayProvenance.hxx"

namespace sf4e { namespace platform { namespace replays {

// The slots a Versus battle saves into: the match list Sidecar gives the game
// (sf4e__Game__Battle.cxx). The stock game uses 300 to 309 and keeps replays
// the native online service handed out in 280 to 299, which are copied too.
constexpr int kFirstMatchSlot = 280, kLastMatchSlot = replayslots::kSlots - 1;

// The game's save folders, one for each Steam account that has played on this
// PC, and the folder the replays are copied to (%APPDATA%\sf4e\replays).
// archive is empty when Windows has no settings folder for this user, saves
// when there is no Steam path or no settings folder.
// active is the one of saves that belongs to the account signed in to the
// running Steam client (its ActiveUser), empty when Steam is not running or
// that account has no saves here. outbox holds replays on their way into the
// archive (%LOCALAPPDATA%, so a failing archive folder does not stop it),
// empty when Windows has no local settings folder.
struct Folders {
	std::vector<std::filesystem::path> saves;
	std::filesystem::path archive, active, outbox;
};
Folders FindFolders();

// Copies every match replay not yet in the archive, one file each, named by
// its save time (UTC) and CRC (ReplayArchivePass.hxx). The archive holds a
// replay when one of its files is that replay, whatever its save time: a
// replay put back into the game is saved there under a new time. Sidecar
// copies the signed-in account's replays as the game saves them: each match
// start and end and each change to the save folder asks for a pass, passes go
// on for two minutes after a match ends, and every thirty seconds while the
// outbox holds replays. A replay goes through the outbox, which it leaves
// once the archive holds it. Its record is made from its own header, never
// taken from the game's index (ReplayArchivePass.hxx: ExportFromFile). This
// is the launcher's sweep before and after a game run, for every account,
// which also empties an outbox a closed game left. Returns how many were copied, or -1 when there is nowhere to copy
// from or to.
int Archive();

// Preparation reads slots/indexes, verifies the backup and encodes recovery on
// the worker. The immutable result is handed once to the game owner. Commit
// rereads exact overwrite targets and the verified backup in the single Apply
// executor's pre-write gate, then performs Steam/live-table
// publication and rollback. Incomplete recovery is retained and persisted by
// the worker, and blocks all further imports. No planning or archive scans in Commit.
struct Imported {
	int slot = -1;
	replayslots::Bytes record, slotBytes;
};
// Failures distinguish rejection, restored originals and incomplete recovery.
enum class ImportResult { Done, NoFolder, NotAReplay, IndexBehind, ArchiveFailed, IndexDamaged, RejectedBeforeWrite, FailedRestored, RecoveryIncomplete };
inline const char* ImportNotice(ImportResult result) {
 switch (result) {
 case ImportResult::IndexBehind: return "replays.not_added_yet";
 case ImportResult::NotAReplay: return "replays.not_added";
 case ImportResult::ArchiveFailed: return "replays.not_added_archive";
 case ImportResult::FailedRestored: return "replays.not_added_restored";
 case ImportResult::RecoveryIncomplete: return "replays.not_added_recovery";
 case ImportResult::Done: return "";
 default: return "replays.not_added_files";
 }
}
using Writer = std::function<bool(const std::string& name, const replayslots::Bytes& contents)>;
using Remover = std::function<bool(const std::string& name)>;
using Publisher = std::function<bool(const Imported& imported)>;
struct PreparedImport {
 ImportResult result = ImportResult::RejectedBeforeWrite;
 Imported imported;
 std::vector<replayfiles::Change> changes;
 replayfiles::BackupEvidence backup;
 std::filesystem::path source, recoveryFile;
 replayslots::Bytes recoveryBytes;
 // Armed before any reads; notifications only invalidate the owned snapshots.
 std::function<bool()> notInvalidated;
 bool Fresh() const {
  return notInvalidated && notInvalidated() && replayfiles::ImportFilesUnchanged(changes, backup);
 }
};
using ImportTransaction = std::shared_ptr<const PreparedImport>;
// Done here means queued; only CommitImport reports a completed import.
ImportResult WantImport(const std::filesystem::path& file);
bool TakeImport(ImportTransaction& out);
ImportResult CommitImport(const ImportTransaction& prepared, const Writer& write, const Remover& remove, const Publisher& publish, bool watched);

// Enqueue recording-boundary facts only. One worker observes the active
// account's slots and binds delayed saves; a missed baseline receives no names.
// On native close, bind a unique newly saved body to these names; no archive-to-time
// join is performed. Native metadata has no verified Ember player/session ID,
// so initial attribution uses account, slot change, fighters and native save
// time within this match's lifetime. Body-bound notes thereafter are exact.
// A tournament game passes its set (SetTag); the names keep it only when
// NoteMatchEnd says its result was confirmed.
// Both also ask for an archive pass (Archive).
void NoteMatchStart(const std::string& p1, const std::string& p2, bool spectating, int fighter1, int fighter2, bool recordingEnabled,
	const replayfiles::SetTag& set = {});
void NoteMatchEnd(bool confirmed);

// Remembers that an archived replay was played with Watch now (watched.txt in
// the archive, one file name per line), so the list can say so.
void MarkWatched(const std::filesystem::path& file);
// The same for a set's replay its bridge has (sent.txt).
void MarkSent(const std::filesystem::path& file);

// Where a set's pack is saved: packs\<matchId>.emberpack in the archive, the
// folder made. Empty when there is no archive or the ID is not a plain one.
std::filesystem::path SetPackPath(const std::string& matchId);
// Puts every game of a set's pack (platform/ReplaySets.hxx) into the archive,
// as games of that set this PC watched. How many the archive holds now; 0 for
// a file that is not a whole pack.
std::size_t ImportSetPack(const std::filesystem::path& pack);

// The archive, newest first. label is the save time in local time, fighters
// the two native fighter IDs from the replay's record (-1 when the file
// holds none), names the players Ember noted (empty when it noted none),
// spectated whether this PC only watched that match, watched whether
// it was played with Watch now before, and sent whether its set's bridge has it.
struct ArchivedReplay {
	std::filesystem::path path;
	std::string label;
	std::uint64_t time = 0;
	int fighters[2] = {-1, -1};
	std::string names[2];
	bool spectated = false, watched = false, sent = false;
	// The tournament set it was a game of, or none.
	replayfiles::SetTag set;
	// An exported video (<name>.mp4) is beside it.
	bool video = false;
	// What the replay itself says of the match (common/ReplayInputs.hxx);
	// none for a file that is not one it reads.
	std::optional<replayinputs::Summary> summary;
};

// Lists the archive on a thread of its own: Ember's own files from the
// archive root, and usf4-replay-saver's (.usf4replay) from any folder under
// it, those with time and fighters from the replay's header and no names.
// Any thread may ask and read: WantListing asks for a listing, which starts
// within two seconds of the one before, and LatestListing is the last one
// made (null before the first). A listing reads bounded files and reuses
// parsed summaries only when their complete contents are unchanged. All of
// that work stays on the worker. StopListing ends
// the thread and waits for a listing in progress; called once, at shutdown,
// and never under the loader lock.
void WantListing();
std::shared_ptr<const std::vector<ArchivedReplay>> LatestListing();
void StopListing();

// Every screen entry has a new revision. Completion exists even when the
// worker cannot allocate a detail. The immutable value is shared by all views.
// Each requester has its own request and completion, so the Replays screen
// and a playback's input lanes (Playback) never take each other's.
enum class DetailFor { Screen, Playback };
void WantDetail(const std::string& file, std::uint64_t revision, DetailFor requester = DetailFor::Screen);
replayinputs::DetailCompletion LatestDetail(DetailFor requester = DetailFor::Screen);

} } }
