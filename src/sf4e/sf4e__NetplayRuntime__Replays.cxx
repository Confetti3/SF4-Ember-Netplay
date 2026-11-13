// Replays: the files handed to the game to import (the picker's, drops', a link's Import) and
// the replay links, taken each tick of the runtime.
#include "sf4e__NetplayRuntime.hxx"
#include "../platform/ReplayFiles.hxx"
#include "../platform/ReplayPath.hxx"
#include "../platform/Utf8.hxx"
#include "sf4e__ReplayStore.hxx"

namespace sf4e { namespace NetplayFacade {
namespace internal {
// No room joined and none being joined: where the battle log may be opened
// and where Ember's menu may come back to the Replays screen.
static bool NoRoom() { return runtime->controller.GetSnapshot().room == netplay::RoomState::Idle && !UserApp::netplay; }

// A replay link's file is played only on the player's word, so any request
// for it, or a dismissal, answers the question the link left.
void RunReplayRequest(const replay::Request& request) {
    if (request.mode == replay::Mode::DismissLink || request.path == runtime->replayLinkAsked) runtime->replayLinkAsked.clear();
    if (request.mode == replay::Mode::Import) {
        // A link's Import joins the drops' and the picker's queue: TickReplays imports it,
        // and only on Ember's menus.
        ReplayImports().Add({request.path});
        return;
    }
    if (request.mode == replay::Mode::Keep) { platform::replays::MarkKept(platform::Utf8ToWide(request.path.c_str()), request.keep); platform::replays::WantListing(); return; }
    replaystore::Start(request, AtMainMenu(), NoRoom());
}

// What a run of imports came to, kept for the Replays screen beside the count of batches that
// ended. The batch is its own answer: the screen needs no other counter to tell it from an
// older one, whether it begins and ends in one tick or many.
static void SayImportBatch(const replay_import::Batch& batch) {
    auto& notice = runtime->importNotice;
    notice.error = batch.failed != 0;
    if (!batch.failed) notice.text = loc::Tf("replays.import_done", batch.held, batch.files);
    else if (batch.files == 1) notice.text = loc::T("replays.set_import_failed");
    else notice.text = loc::Tf("replays.import_some_failed", batch.failed, batch.files);
}

void TickReplays() {
    ApplyGameDisplay();
    // Games downloaded from the public archive to watch last only as long as this run.
    static bool cleared = false;
    if (!cleared) { cleared = true; platform::replays::ClearArchiveWatching(); }
    // What the picker returned joins what drops and links handed over.
    ReplayImports().Add(runtime->services.TakePicked());
    // One file a tick, read and written on the game thread (a pack is at most a couple of
    // MB), and only on Ember's menus: in a room or a match the files wait.
    if (const auto batch = replay_import::Step(ReplayImports(), AtMainMenu(), NoRoom(), [](const std::string& file) {
            return platform::replays::ImportReplayFile(platform::Utf8ToWide(file.c_str()));
        })) SayImportBatch(*batch);
    const std::string link = platform::ResolveReplayFile(runtime->replayLinks.Take());
    if (!link.empty()) {
        runtime->replayLinkAsked = link;
        spdlog::info("Replays: a link asked for a replay; the Replays screen asks the player");
    }
    replaystore::Tick(AtMainMenu(), NoRoom());
}
} // namespace internal
} } // namespace sf4e::NetplayFacade
