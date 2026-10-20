#pragma once
#include "../common/WipeText.hxx"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace sf4e { namespace netplay {
// One identity or bridge request from the interface. The runtime turns it into
// the helper's tournament JSON, so the interface never writes helper JSON and
// a request the helper would not parse cannot be built.
enum class IdentityOp : std::uint8_t {
    None, Status, Enable, Unlock, Export, PreviewImport, Import, Reset,
    BridgeList, BridgeInspect, BridgeApprove, BridgeForget,
    LinkList, LinkClaim, LinkCancel, LinkRemove,
    DiscordStatus, DiscordConnect, DiscordRemove, DiscordCancel,
    SiteStatus, SiteConnect, SiteRemove, SiteCancel,
    // The tournament desk: a read of the organizer's events or one event's
    // sets (`target`: the event, or none), and an action at `target`, a desk
    // path, with `command` (JSON) as its body.
    DeskRead, DeskAct,
    // The player's web desk sessions: a read, noting the game told the
    // player about them, and signing them all out.
    DeskWeb, DeskWebSeen, DeskWebEnd,
    // Opening the web desk from the game: in the browser, or as a code to
    // type on another device, read first for what asked and the numbers to
    // pick from, then approved with the number that device shows.
    DeskOpen, DeskApprove, DeskCodeInfo,
};

// Where a player connects an account to their Ember ID through a browser
// sign-in on a tournament service: Discord, or one of the sign-in sites the
// service lists (Challonge, start.gg and later ones, each named by its ID in
// the request's `site`). Each has four requests: read the account, start a
// sign-in (the helper opens the site's page), disconnect, and end a sign-in.
enum class SignInSource : std::uint8_t { Discord, Site };
enum class SignInStep : std::uint8_t { Status, Connect, Remove, Cancel };
constexpr std::size_t SignInSourceCount = 2, SignInStepCount = 4;
struct SignInOp { SignInSource source; SignInStep step; };
constexpr IdentityOp SignInOps[SignInSourceCount][SignInStepCount] = {
    {IdentityOp::DiscordStatus, IdentityOp::DiscordConnect, IdentityOp::DiscordRemove, IdentityOp::DiscordCancel},
    {IdentityOp::SiteStatus, IdentityOp::SiteConnect, IdentityOp::SiteRemove, IdentityOp::SiteCancel},
};
constexpr IdentityOp SignInRequest(SignInSource source, SignInStep step) {
    return SignInOps[static_cast<std::size_t>(source)][static_cast<std::size_t>(step)];
}
// The source and step a request is; none for any other request.
constexpr std::optional<SignInOp> SignInOf(IdentityOp op) {
    for (std::size_t source = 0; source < SignInSourceCount; ++source)
        for (std::size_t step = 0; step < SignInStepCount; ++step)
            if (SignInOps[source][step] == op) return SignInOp{static_cast<SignInSource>(source), static_cast<SignInStep>(step)};
    return std::nullopt;
}

// What a request does, the one classification the runtime and the interface
// both use. A read asks the helper or a service and changes nothing, so it
// goes through during a live game. A preview is key work that only shows
// something (a backup's contents). A key change creates, replaces or retires
// the key, only outside a room. An open starts a site's sign-in in the
// browser. A cleanup ends a sign-in the player cancelled or left, and nobody
// waits for its answer. Anything else changes what the player asked to.
enum class IdentityEffect : std::uint8_t { Read, Preview, KeyChange, Open, Cleanup, Change };
constexpr IdentityEffect EffectOf(IdentityOp op) {
    if (const auto signIn = SignInOf(op)) {
        switch (signIn->step) {
        case SignInStep::Status: return IdentityEffect::Read;
        case SignInStep::Connect: return IdentityEffect::Open;
        case SignInStep::Cancel: return IdentityEffect::Cleanup;
        case SignInStep::Remove: return IdentityEffect::Change;
        }
    }
    switch (op) {
    case IdentityOp::Status: case IdentityOp::BridgeList: case IdentityOp::BridgeInspect: case IdentityOp::LinkList:
    case IdentityOp::DeskRead: case IdentityOp::DeskWeb: case IdentityOp::DeskCodeInfo:
        return IdentityEffect::Read;
    case IdentityOp::DeskOpen: return IdentityEffect::Open;
    case IdentityOp::PreviewImport: return IdentityEffect::Preview;
    case IdentityOp::Enable: case IdentityOp::Import: case IdentityOp::Reset: return IdentityEffect::KeyChange;
    default: return IdentityEffect::Change;
    }
}

struct IdentityRequest {
    IdentityOp op = IdentityOp::None;
    // Chosen by the interface, so it can tell its request's answer apart.
    std::uint64_t ticket = 0;
    // Secrets. Wipe() clears them; every holder wipes its copy when done.
    std::string passphrase, localPassphrase;
    // The backup to read. An export's file is chosen by the runtime.
    std::string path;
    std::string origin, bridge, connection, code;
    // The claim or link to cancel or remove.
    std::string target;
    // The sign-in site the site requests are for, by its ID, and whether its
    // status, connect and remove are the organizer sign-in's.
    std::string site;
    bool organizer = false;
    // Import: the ID its preview showed; Reset: the ID being retired.
    std::string emberId;
    bool replace = false;
    // A desk action's body, as JSON.
    std::string command;
    // DeskApprove: the number the other device shows.
    std::uint8_t number = 0;

    static constexpr std::size_t MaxSecret = 1024, MaxPath = 1024, MaxField = 256;
    bool Valid() const {
        return passphrase.size() <= MaxSecret && localPassphrase.size() <= MaxSecret && path.size() <= MaxPath &&
            origin.size() <= MaxField && bridge.size() <= MaxField && connection.size() <= MaxField &&
            code.size() <= MaxField && target.size() <= MaxField && emberId.size() <= MaxField && site.size() <= MaxField &&
            command.size() <= MaxPath;
    }
    std::size_t Bytes() const {
        return passphrase.size() + localPassphrase.size() + path.size() + origin.size() + bridge.size() +
            connection.size() + code.size() + target.size() + emberId.size() + site.size() + command.size();
    }
    // Every copy wipes its secrets when it goes, including one a full queue refused.
    IdentityRequest() = default;
    IdentityRequest(const IdentityRequest&) = default;
    IdentityRequest(IdentityRequest&&) = default;
    IdentityRequest& operator=(const IdentityRequest&) = default;
    IdentityRequest& operator=(IdentityRequest&&) = default;
    ~IdentityRequest() { Wipe(); }
    void Wipe() { WipeText(passphrase); WipeText(localPassphrase); }
};
} }
