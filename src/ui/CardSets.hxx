#pragma once
// A card's recent sets: the one place the card uses the public archive. It asks
// the archive's second slot (ShellView::profileSets) for the games with a side
// of one exact name, groups them as the Archive tab does (ArchiveSets.hxx), says
// each set as a row (player_card::SetRow), and starts a set's games watching
// through the shared queue (WatchQueue.hxx). The own card and, later, another
// player's page each hold one.
//
// The archive's types and calls it uses, for whoever reconciles them with the
// archive's own panel: commands::ArchiveList (profile = true) with
// netplay::archive::Filter::Named; ShellView::profileSets and its
// archive::Status::Answers; archivesets::Group, Entry, SideOf and Score;
// WatchQueue::Fill and Start (commands::ArchiveWatch); replay::WatchCaption.
#include "CardModel.hxx"
#include "ArchiveSets.hxx"
#include "MenuNavigation.hxx"
#include "ShellAction.hxx"
#include "WatchQueue.hxx"
#include "../netplay/ArchiveCommands.hxx"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct ShellView;

namespace card_sets {
// The row a set has: "card-set:" and the entry's id.
constexpr const char* RowPrefix = "card-set:";
inline bool IsRow(const std::string& id) { return id.compare(0, 9, RowPrefix) == 0; }

// One set as a card row says it, for the player called `name`: their fighter and the opponent's from the set's last
// game, the opponent's name, flag and class, and "3 – 1 · Sample Cup · Top 8 · 2 d ago" (a tournament set's score from
// the player's side), "2 games · Online · 2 d ago" for the other sets, which the listing holds no score of. `now` is unix
// seconds (0 when unknown).
player_card::SetRow RowOf(const archivesets::Entry& entry, const std::string& name, std::uint64_t now);
// The sets part of the card for a listing's state: reading while the first page is on its way, the rows, none, the
// service keeps no archive, or unreachable. `answers` says the status is the listing of the wanted name.
player_card::SetsView ViewOf(const netplay::archive::Status& status, bool answers, const std::vector<archivesets::Entry>& entries, const std::string& name,
    std::uint64_t now);
}

class CardSets {
public:
    using Submit = std::function<bool(ShellAction)>;
    // Every frame: the service the archive is read from (empty when none), the exact name to list, and whether the
    // card is on show. The first page is asked for when the card comes on show, and again when the name or service
    // changes; never on every frame, and never while it is not shown.
    void Update(const ShellView& view, const std::string& bridge, const std::string& name, bool shown, const Submit& submit);
    // The sets part, as of the listing last heard.
    player_card::SetsView View(const ShellView& view) const;
    // Select on a set's row: its games are queued and the first starts. False when `id` is not a set's row.
    bool Activate(const std::string& id, const ShellView& view, const Submit& submit, WatchQueue& queue, std::string& error);

private:
    void Regroup(const ShellView& view);

    std::string bridge_, name_;
    commands::ArchiveList last_;
    std::uint64_t revision_ = 0;
    bool retry_ = false, visiting_ = false;
    std::shared_ptr<const netplay::archive::Listing> held_;
    std::vector<archivesets::Entry> entries_;
};

} }
