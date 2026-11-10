#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace netplay {
// The player's Ember identity and linked accounts as the helper last reported
// them. Public values only: no key, passphrase or bridge token reaches it, so
// it is copied freely into snapshots and the overlay.
// A trusted service. A built-in one comes with this build of Ember: the
// player trusts it without approving it, and can still remove it. It stays
// dormant, out of the player's services, until they use it (active).
struct IdentityBridge { std::string id, origin, name; bool builtin = false, active = false; };
struct IdentityConnection { std::string id, name; };
// A linked account (id is the link) or a claim waiting for approval (id is the claim).
struct IdentityLink { std::string id, connection, provider, account; };
// An account connected on a sign-in service: its user ID (empty when none),
// name and when it connected, in seconds; each sign-in connects it anew.
struct IdentityAccount {
    std::string user, name;
    std::uint64_t connectedAt = 0;
    // Connected after `before`: another account, or the same one again (a
    // service sending no time reads 0 throughout, so that waits out its deadline).
    bool ConnectedSince(const IdentityAccount& before) const {
        return !user.empty() && (user != before.user || connectedAt != before.connectedAt);
    }
};
// The tournament desk on the bracket site: one of the organizer's
// tournaments, and one set of it with what the desk shows of it (state: the
// helper's desk_state). A set's games are what the desk can decide or void;
// revision is what a decision names. startsAt 0 and cap 0 are not set;
// webhook: a Discord channel is set.
struct DeskEvent {
    // `status`: the bracket site's state; `actions`: the moves it allows now.
    std::string id, title, status;
    std::uint64_t startsAt = 0, signedUp = 0, cap = 0;
    int gamesToWin = 2;
    bool checkIn = false, webhook = false;
    std::vector<std::string> actions;
};
struct DeskGame { std::string attempt, outcome, state; };
struct DeskSet {
    std::string id, roundLabel, state, matchId, revision;
    std::string players[2];
    int score[2] = {0, 0};
    int round = 0;
    bool held = false, scored = false;
    std::vector<DeskGame> games;
};
// Whether the site takes organizers by invitation, and whether this
// player redeemed one.
struct DeskAccess { bool known = false, inviteOnly = false, allowed = true; };
struct IdentityView {
    // A status has arrived from the helper.
    bool known = false;
    // disabled, creating, locked, ready, unavailable or recovery_required.
    std::string state, reason, emberId, fingerprint, backend;
    bool passphraseRequired = false;
    std::vector<IdentityBridge> bridges;
    // Services the player removed: never trusted again by themselves.
    std::vector<std::string> removedBridges;
    // The bridge last inspected or listed, with the connections it offers,
    // whether it offers Discord sign-in, and whether a connected Discord
    // account can be read and disconnected there (with or without sign-in).
    IdentityBridge inspected;
    std::vector<IdentityConnection> connections;
    bool inspectedDiscord = false, inspectedDiscordAccounts = false;
    // Whether it names a bracket site for the tournament desk.
    bool inspectedDesk = false;
    // The Discord account connected on the bridge last asked; empty when none.
    std::string discordUser, discordName;
    std::uint64_t discordConnectedAt = 0;
    IdentityAccount Discord() const { return {discordUser, discordName, discordConnectedAt}; }
    // Links and pending claims on the bridge last listed.
    std::vector<IdentityLink> links, pending;
    // The last answer: which request, what it did, and a stable failure code.
    std::uint64_t requestId = 0;
    std::string op, failure;
    bool ok = false;
    // A checked backup: the ID it holds and what restoring it would do.
    std::string previewEmberId, previewFingerprint;
    bool previewSame = false, previewReplaces = false;
    // The fingerprint the bridge recorded for the last claim, and the last backup written.
    std::string claimFingerprint, exportPath;
    // The desk's last reads: the organizer's events, and the sets of deskEvent.
    std::vector<DeskEvent> deskEvents;
    std::string deskEvent;
    std::vector<DeskSet> deskSets;
    // Tournament Mode on the site: known once the desk was read.
    DeskAccess deskAccess;
    // The tournament the last desk action made, by ID.
    std::string deskMade;
};
} }
