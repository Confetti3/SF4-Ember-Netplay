#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
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
// A sign-in site a service lists: its ID, its name to show, whether it
// offers sign-in now, and whether it offers an organizer sign-in too. Which
// pages its sign-in may open is the helper's to check, from the same list.
struct IdentitySignInSite { std::string id, name; bool signIn = false, organizer = false; };
// The most sign-in sites kept from one service's list.
constexpr std::size_t MaxSignInSites = 16;
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
// The tournament desk: one of the organizer's events, and one set of an
// event with what the desk shows of it (side: winners, losers or
// grand_final; state: the bridge's desk_state). A set's games are what the
// desk can decide or void; revision is what a decision names.
struct DeskEvent { std::string id, title, status, site; };
struct DeskGame { std::string attempt, outcome, state; };
struct DeskSet {
    std::string id, roundLabel, phase, side, state, matchId, revision, posted;
    std::string players[2];
    int score[2] = {0, 0};
    int round = 0;
    bool held = false, scored = false;
    std::uint64_t calledAt = 0;
    std::vector<DeskGame> games;
};
// A web desk session of the player's: the site it signed in with, the
// browser's short name, when it started and was last used, and whether the
// game has told the player about it.
struct WebSession {
    std::string site, browser;
    std::uint64_t startedAt = 0, lastUsedAt = 0;
    bool fresh = false;
};
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
    // The sign-in sites the bridge last inspected lists.
    std::vector<IdentitySignInSite> signInSites;
    // The Discord account connected on the bridge last asked, and the account
    // connected on the sign-in site last asked; empty when none.
    std::string discordUser, discordName, siteUser, siteName;
    std::uint64_t discordConnectedAt = 0, siteConnectedAt = 0;
    // Whether the sign-in site last asked holds this Ember ID's organizer
    // grant, from an organizer read.
    bool siteOrganizer = false;
    IdentityAccount Discord() const { return {discordUser, discordName, discordConnectedAt}; }
    IdentityAccount Site() const { return {siteUser, siteName, siteConnectedAt}; }
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
    // The player's web desk sessions, newest first.
    std::vector<WebSession> webSessions;
};
} }
