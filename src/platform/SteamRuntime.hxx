#pragma once

// The narrow wrapper around USF4's already-loaded steam_api.dll (legacy
// SteamUser017/SteamFriends014). See docs/design/STEAM_IDENTITY_AND_INVITES.md,
// "Steam runtime wrapper". Ember never loads or initializes Steam itself: it
// only reads the interfaces the game already created, and only after the
// game's own Steam initialization has completed.
//
// Attach() is the only OS-dependent entry point (GetModuleHandleW/GetProcAddress,
// implemented in SteamRuntime.cxx). Every other method operates purely on the
// already-resolved interface pointers, so it can be exercised in a unit test
// with a fake ISteamUser017/ISteamFriends014 instead of the real DLL; see
// src/tests/steam_runtime_test.cxx.

#include "SteamAbi.hxx"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace sf4e {
namespace steam {

struct LocalIdentity {
	bool available = false;
	bool loggedOn = false;
	std::uint64_t steamId = 0;
	std::string personaName;
};

struct Friend {
	std::uint64_t steamId = 0;
	std::string personaName;
};

// A join accepted through Rich Presence or a direct invite, read off the
// bounded mailbox the callback fills. Never trusted as authorization: normal
// room join checks still apply once the code reaches the join flow.
struct JoinRequest {
	std::uint64_t friendSteamId = 0;
	std::string code;
};

// Friend enumeration is capped even if Steam reports more, so a corrupt or
// unusually large friends list cannot make Ember allocate without bound.
static const int MaximumFriendRows = 500;

class Runtime {
public:
	// Resolves steam_api.dll's exported accessors and legacy interfaces from
	// the module the game already loaded. Returns false, leaving Steam
	// features unavailable, if the module or an export is missing, if the
	// accessors return null, or if the local account does not look valid.
	// Windows-specific; implemented in SteamRuntime.cxx.
	bool Attach();

	// The logic Attach() runs once it has interface pointers, split out so a
	// test can supply a fake ISteamUser017/ISteamFriends014 without the real
	// DLL. Does not take ownership of either pointer: both must outlive the
	// Runtime (USF4 owns their lifetime; Steam frees them at its own shutdown).
	bool AttachWithInterfaces(steam_abi::ISteamUser017* user, steam_abi::ISteamFriends014* friends);

	void Detach();
	bool Attached() const { return user_ != nullptr && friends_ != nullptr; }

	// Copies the local identity immediately; no Steam pointer is retained.
	LocalIdentity LocalUser() const;

	// Copies up to MaximumFriendRows immediate friends. Never used to
	// identify an invite target by name: callers keep the SteamID.
	std::vector<Friend> Friends() const;

	// Publishes "status" and "connect" only when either differs from the
	// last published value, so Steam is not called every frame. `connect`
	// may be empty to publish status with no joinable connect value.
	bool Publish(const std::string& status, const std::string& connect);

	// Targeted clears ("connect" then "status" set to ""), not
	// ClearRichPresence(), so Ember never erases values USF4 itself may own.
	void ClearPresence();

	// Requires a validated individual SteamID and a nonempty connect string
	// at or under Rich Presence's 256-byte value limit.
	bool Invite(std::uint64_t friendSteamId, const std::string& connect);

	// Takes the newest pending join request, if any, clearing the mailbox.
	bool TakeJoinRequest(JoinRequest& out);

	// Called by the registered callback (or directly by a test) with the
	// callback's own 256-byte buffer. Parses strictly and keeps only the
	// newest request; never logs the connect string.
	void OnJoinRequested(std::uint64_t friendSteamId, const char* connectBuffer, std::size_t bufferSize);

private:
	steam_abi::ISteamUser017* user_ = nullptr;
	steam_abi::ISteamFriends014* friends_ = nullptr;
	std::string publishedStatus_, publishedConnect_;
	bool published_ = false;

	mutable std::mutex mailboxMutex_;
	bool mailboxHasRequest_ = false;
	JoinRequest mailboxRequest_;

	// Opaque from this header's point of view (the concrete class lives in
	// SteamRuntime.cxx); only Detach() unregisters and deletes it.
	steam_abi::CCallbackBase* callback_ = nullptr;
	void* registerCallbackFn_ = nullptr;
	void* unregisterCallbackFn_ = nullptr;
};

} // namespace steam
} // namespace sf4e
