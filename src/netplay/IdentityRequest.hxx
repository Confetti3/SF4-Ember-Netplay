#pragma once
#include "../common/WipeText.hxx"
#include <cstddef>
#include <cstdint>
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
};
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
    // Import: the ID its preview showed; Reset: the ID being retired.
    std::string emberId;
    bool replace = false;

    static constexpr std::size_t MaxSecret = 1024, MaxPath = 1024, MaxField = 256;
    bool Valid() const {
        return passphrase.size() <= MaxSecret && localPassphrase.size() <= MaxSecret && path.size() <= MaxPath &&
            origin.size() <= MaxField && bridge.size() <= MaxField && connection.size() <= MaxField &&
            code.size() <= MaxField && target.size() <= MaxField && emberId.size() <= MaxField;
    }
    std::size_t Bytes() const {
        return passphrase.size() + localPassphrase.size() + path.size() + origin.size() + bridge.size() +
            connection.size() + code.size() + target.size() + emberId.size();
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
