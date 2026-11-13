#pragma once
// A set's games still to start, one at a time: the one queue behind Replays'
// Next game row, whether the set is on this PC (Sets) or in the public archive
// (Archive). A game is a file to watch; an archive game is downloaded first,
// by the command that starts it, which then watches the file. With
// `exporting` the same games are made into videos instead (ReplaysPanel).
#include "ShellAction.hxx"
#include "ShellView.hxx"
#include "../common/ReplayRequest.hxx"
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace ui {

struct QueuedGame {
    // A game's file on this PC, or else the archive game to download:
    // its service, address and digest.
    std::string path, bridge, url, digest;
    // What Watch draws over it (replay::WatchCaption).
    replay::Caption caption;
    bool Archived() const { return !digest.empty(); }
};

class WatchQueue {
public:
    // The set's games, in the order they are played.
    void Fill(std::vector<QueuedGame> games, bool exporting = false) { games_ = std::move(games); total_ = games_.size(); exporting_ = exporting; }
    void Clear() { games_.clear(); }
    bool Empty() const { return games_.empty(); }
    // Whether the games are to be exported (and still some are).
    bool Exporting() const { return exporting_ && !games_.empty(); }
    // Which game the front one is, of how many.
    std::size_t Position() const { return total_ - games_.size() + 1; }
    std::size_t Total() const { return total_; }
    const QueuedGame& Front() const { return games_.front(); }
    void Pop() { games_.erase(games_.begin()); }
    // Whether the front game can start now: the game takes a replay, and an
    // archive game is not still being downloaded behind another.
    bool Ready(const ShellView& v) const { return !games_.empty() && v.replays.ready && (!Front().Archived() || v.archiveBrowse.fetching.empty()); }
    // The command that starts the front game, which then leaves the queue.
    ShellAction Start(const ShellView& v) {
        const QueuedGame game = std::move(games_.front());
        games_.erase(games_.begin());
        replay::Request watch;
        watch.mode = replay::Mode::Watch;
        watch.caption = game.caption;
        if (!game.Archived()) {
            commands::Replay command;
            command.generation = v.session.generation;
            watch.path = game.path;
            command.replay = std::move(watch);
            return command;
        }
        commands::ArchiveWatch command;
        command.bridge = game.bridge; command.url = game.url; command.digest = game.digest;
        command.watch = std::move(watch);
        return command;
    }

private:
    std::vector<QueuedGame> games_;
    std::size_t total_ = 0;
    bool exporting_ = false;
};

} }
