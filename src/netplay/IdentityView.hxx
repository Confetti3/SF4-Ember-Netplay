#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sf4e { namespace netplay {
// The player's Ember identity and linked accounts as the helper last reported
// them. Public values only: no key, passphrase or bridge token reaches it, so
// it is copied freely into snapshots and the overlay.
struct IdentityBridge { std::string id, origin, name; };
struct IdentityConnection { std::string id, name; };
// A linked account (id is the link) or a claim waiting for approval (id is the claim).
struct IdentityLink { std::string id, connection, provider, account; };
struct IdentityView {
    // A status has arrived from the helper.
    bool known = false;
    // disabled, creating, locked, ready, unavailable or recovery_required.
    std::string state, reason, emberId, fingerprint, backend;
    bool passphraseRequired = false;
    std::vector<IdentityBridge> bridges;
    // The bridge last inspected or listed, with the connections it offers.
    IdentityBridge inspected;
    std::vector<IdentityConnection> connections;
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
};
} }
