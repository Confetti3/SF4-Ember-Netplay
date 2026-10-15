#pragma once

// The exact legacy Steamworks ABI USF4 ships (steam_api.dll, interface
// versions SteamUser017 / SteamFriends014, see
// docs/design/STEAM_IDENTITY_AND_INVITES.md, "Observed USF4 Steam API
// runtime"). This is NOT the modern Steamworks SDK: its vtables are laid out
// differently and must not be used to call into this DLL.
//
// Only the methods Ember actually calls are given real signatures. Every
// other virtual method in each interface, up to the last one Ember uses, is
// declared as an unused pure-virtual placeholder purely to keep vtable slot
// numbers aligned with the real interface. Ember never implements these
// interfaces: it only reinterprets a pointer the game's own Steam API
// returned, so the placeholders need no bodies and are never called.
//
// x86 only: USF4 and its Steam API are 32-bit. Do not include this header
// from 64-bit code (the Discord companion process, for example).

#include <cstddef>
#include <cstdint>

namespace sf4e {
namespace steam_abi {

#pragma pack(push, 4)

// CSteamID's real definition is a bitfield union; Ember only ever moves the
// raw 64-bit value, so a single member is layout-compatible for every call
// that passes or returns a CSteamID by value.
struct CSteamID {
	std::uint64_t value = 0;
	CSteamID() = default;
	explicit CSteamID(std::uint64_t v) : value(v) {}
	std::uint64_t ConvertToUint64() const { return value; }
};
static_assert(sizeof(CSteamID) == 8, "CSteamID must match Steam's 64-bit handle");

using SteamAPICall_t = std::uint64_t;

#pragma pack(pop)

// SteamUser017: only BLoggedOn (slot 1) and GetSteamID (slot 2) are used.
class ISteamUser017 {
public:
	virtual void Unused00() = 0;
	virtual bool BLoggedOn() = 0;
	virtual CSteamID GetSteamID() = 0;
};

// SteamFriends014: GetPersonaName, friend enumeration, Rich Presence and
// InviteUserToGame. Every other slot up to 43 is an unused placeholder so
// the ones Ember calls land on the correct vtable entry.
class ISteamFriends014 {
public:
	virtual const char* GetPersonaName() = 0;                                   // slot 0
	virtual void Unused01() = 0;
	virtual void Unused02() = 0;
	virtual int GetFriendCount(int iFriendFlags) = 0;                           // slot 3
	virtual CSteamID GetFriendByIndex(int iFriend, int iFriendFlags) = 0;       // slot 4
	virtual void Unused05() = 0;
	virtual void Unused06() = 0;
	virtual const char* GetFriendPersonaName(CSteamID steamIDFriend) = 0;       // slot 7
	virtual void Unused08() = 0;
	virtual void Unused09() = 0;
	virtual void Unused10() = 0;
	virtual void Unused11() = 0;
	virtual void Unused12() = 0;
	virtual void Unused13() = 0;
	virtual void Unused14() = 0;
	virtual void Unused15() = 0;
	virtual void Unused16() = 0;
	virtual void Unused17() = 0;
	virtual void Unused18() = 0;
	virtual void Unused19() = 0;
	virtual void Unused20() = 0;
	virtual void Unused21() = 0;
	virtual bool ActivateGameOverlay(const char* pchDialog) = 0;                // slot 22
	virtual void Unused23() = 0;
	virtual void Unused24() = 0;
	virtual void Unused25() = 0;
	virtual void Unused26() = 0;
	virtual void Unused27() = 0;
	virtual void Unused28() = 0;
	virtual void Unused29() = 0;
	virtual void Unused30() = 0;
	virtual bool RequestUserInformation(CSteamID steamIDUser, bool bRequireNameOnly) = 0; // slot 31
	virtual void Unused32() = 0;
	virtual void Unused33() = 0;
	virtual void Unused34() = 0;
	virtual void Unused35() = 0;
	virtual void Unused36() = 0;
	virtual bool SetRichPresence(const char* pchKey, const char* pchValue) = 0; // slot 37
	virtual void ClearRichPresence() = 0;                                      // slot 38
	virtual void Unused39() = 0;
	virtual void Unused40() = 0;
	virtual void Unused41() = 0;
	virtual void Unused42() = 0;
	virtual bool InviteUserToGame(CSteamID steamIDFriend, const char* pchConnectString) = 0; // slot 43
};

// The legacy callback base (steam_api.h's CCallbackBase), stable across the
// 2013-era Steamworks SDK. Ember derives exactly one callback from it; USF4's
// own callback pump invokes Run() through SteamAPI_RunCallbacks(), which
// Ember never calls itself.
class CCallbackBase {
public:
	CCallbackBase() : m_nCallbackFlags(0), m_iCallback(0) {}
	virtual ~CCallbackBase() = default;
	virtual void Run(void* pvParam) = 0;
	virtual void Run(void* pvParam, bool bIOFailure, SteamAPICall_t hSteamAPICall) = 0;
	int GetICallback() const { return m_iCallback; }
	virtual int GetCallbackSizeBytes() = 0;
protected:
	std::uint8_t m_nCallbackFlags;
	int m_iCallback;
};

#pragma pack(push, 4)
// Delivered when a friend accepts an invite or clicks "Join Game" from Rich
// Presence. m_rgchConnect is not guaranteed NUL-terminated by Steam within
// its bound; see steam_join::FromFixedBuffer for how Ember reads it.
struct GameRichPresenceJoinRequested_t {
	enum { k_iCallback = 300 + 37 }; // k_iSteamFriendsCallbacks (300) + 37
	CSteamID m_steamIDFriend;
	char m_rgchConnect[256];
};
static_assert(sizeof(GameRichPresenceJoinRequested_t) == 264, "must match Steam's packed callback layout");
#pragma pack(pop)

} // namespace steam_abi
} // namespace sf4e
