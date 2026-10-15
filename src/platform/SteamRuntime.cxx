#include "SteamRuntime.hxx"

#include "../common/SteamJoin.hxx"
#include "../netplay/SteamIdentity.hxx"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>

namespace sf4e {
namespace steam {

using steam_abi::CSteamID;
using steam_abi::GameRichPresenceJoinRequested_t;
using steam_abi::ISteamFriends014;
using steam_abi::ISteamUser017;

namespace {
// Rich Presence's own documented limits (SteamFriends014): refuse rather
// than silently truncate a value that would be rejected or mangled anyway.
const std::size_t MaximumPresenceKeyBytes = 64;
const std::size_t MaximumPresenceValueBytes = 256;

bool WithinPresenceLimits(const std::string& key, const std::string& value) {
	return !key.empty() && key.size() < MaximumPresenceKeyBytes && value.size() < MaximumPresenceValueBytes;
}
} // namespace

bool Runtime::AttachWithInterfaces(ISteamUser017* user, ISteamFriends014* friends) {
	Detach();
	if (user == nullptr || friends == nullptr) return false;
	if (!user->BLoggedOn()) return false;
	const std::uint64_t steamId = user->GetSteamID().ConvertToUint64();
	if (!steam_identity::IsValidIndividualSteamId(steamId)) return false;
	user_ = user;
	friends_ = friends;
	return true;
}

void Runtime::Detach() {
	if (callback_ != nullptr) {
		if (unregisterCallbackFn_ != nullptr)
			reinterpret_cast<void(__cdecl*)(steam_abi::CCallbackBase*)>(unregisterCallbackFn_)(callback_);
		delete callback_;
		callback_ = nullptr;
	}
	registerCallbackFn_ = nullptr;
	unregisterCallbackFn_ = nullptr;
	user_ = nullptr;
	friends_ = nullptr;
	published_ = false;
	publishedStatus_.clear();
	publishedConnect_.clear();
}

LocalIdentity Runtime::LocalUser() const {
	LocalIdentity out;
	if (user_ == nullptr || friends_ == nullptr) return out;
	out.loggedOn = user_->BLoggedOn();
	if (!out.loggedOn) return out;
	out.steamId = user_->GetSteamID().ConvertToUint64();
	if (!steam_identity::IsValidIndividualSteamId(out.steamId)) return out;
	const char* name = friends_->GetPersonaName();
	out.personaName = name != nullptr ? steam_identity::SanitizePersonaName(name) : std::string();
	out.available = true;
	return out;
}

std::vector<Friend> Runtime::Friends() const {
	std::vector<Friend> out;
	if (friends_ == nullptr) return out;
	// k_EFriendFlagImmediate: real-life friends, not clan members or blocked users.
	const int kFriendFlagImmediate = 0x04;
	int count = friends_->GetFriendCount(kFriendFlagImmediate);
	if (count < 0) return out;
	count = std::min(count, MaximumFriendRows);
	out.reserve(static_cast<std::size_t>(count));
	for (int i = 0; i < count; ++i) {
		const CSteamID id = friends_->GetFriendByIndex(i, kFriendFlagImmediate);
		const std::uint64_t steamId = id.ConvertToUint64();
		if (!steam_identity::IsValidIndividualSteamId(steamId)) continue;
		const char* name = friends_->GetFriendPersonaName(id);
		Friend row;
		row.steamId = steamId;
		row.personaName = name != nullptr ? steam_identity::SanitizePersonaName(name) : std::string();
		out.push_back(std::move(row));
	}
	return out;
}

bool Runtime::Publish(const std::string& status, const std::string& connect) {
	if (friends_ == nullptr) return false;
	if (!WithinPresenceLimits("status", status) || (!connect.empty() && !WithinPresenceLimits("connect", connect))) return false;
	if (published_ && status == publishedStatus_ && connect == publishedConnect_) return true; // nothing changed: no Steam call
	bool ok = friends_->SetRichPresence("status", status.c_str());
	ok = friends_->SetRichPresence("connect", connect.c_str()) && ok;
	if (ok) { publishedStatus_ = status; publishedConnect_ = connect; published_ = true; }
	return ok;
}

void Runtime::ClearPresence() {
	if (friends_ == nullptr) return;
	// Targeted clears, not ClearRichPresence(): see SteamRuntime.hxx.
	friends_->SetRichPresence("connect", "");
	friends_->SetRichPresence("status", "");
	published_ = false;
	publishedStatus_.clear();
	publishedConnect_.clear();
}

bool Runtime::Invite(std::uint64_t friendSteamId, const std::string& connect) {
	if (friends_ == nullptr) return false;
	if (!steam_identity::IsValidIndividualSteamId(friendSteamId)) return false;
	if (connect.empty() || connect.size() >= MaximumPresenceValueBytes) return false;
	return friends_->InviteUserToGame(CSteamID(friendSteamId), connect.c_str());
}

bool Runtime::TakeJoinRequest(JoinRequest& out) {
	std::lock_guard<std::mutex> lock(mailboxMutex_);
	if (!mailboxHasRequest_) return false;
	out = mailboxRequest_;
	mailboxHasRequest_ = false;
	mailboxRequest_ = JoinRequest();
	return true;
}

void Runtime::OnJoinRequested(std::uint64_t friendSteamId, const char* connectBuffer, std::size_t bufferSize) {
	const std::string code = steam_join::FromFixedBuffer(connectBuffer, bufferSize);
	if (code.empty()) return;
	std::lock_guard<std::mutex> lock(mailboxMutex_);
	mailboxHasRequest_ = true;
	mailboxRequest_.friendSteamId = friendSteamId;
	mailboxRequest_.code = code;
}

namespace {
// SteamAPI_RegisterCallback/UnregisterCallback and the legacy interface
// accessors are plain cdecl exports of steam_api.dll (see
// docs/design/STEAM_IDENTITY_AND_INVITES.md, "Observed USF4 Steam API
// runtime"). Resolved by name only from the module the game already loaded;
// Ember never calls LoadLibrary or SteamAPI_Init itself.
using AccessorFn = void*(__cdecl*)();
using RegisterCallbackFn = void(__cdecl*)(steam_abi::CCallbackBase*, int);
using UnregisterCallbackFn = void(__cdecl*)(steam_abi::CCallbackBase*);

// Copies the callback's connect field into the owning Runtime's mailbox and
// nothing else: no room API, no allocation beyond the one copy, no logging
// of the connect string, no call back into Steam.
class JoinRequestCallback : public steam_abi::CCallbackBase {
public:
	explicit JoinRequestCallback(Runtime* owner) : owner_(owner) {
		m_iCallback = GameRichPresenceJoinRequested_t::k_iCallback;
	}
	void Run(void* param) override {
		const auto* data = static_cast<const GameRichPresenceJoinRequested_t*>(param);
		if (owner_ != nullptr && data != nullptr)
			owner_->OnJoinRequested(data->m_steamIDFriend.ConvertToUint64(), data->m_rgchConnect, sizeof(data->m_rgchConnect));
	}
	void Run(void*, bool, steam_abi::SteamAPICall_t) override {} // unused: not a SteamAPICall-based callback
	int GetCallbackSizeBytes() override { return sizeof(GameRichPresenceJoinRequested_t); }
private:
	Runtime* owner_;
};
} // namespace

bool Runtime::Attach() {
	Detach();
	const HMODULE module = GetModuleHandleW(L"steam_api.dll");
	if (module == nullptr) return false; // not loaded: USF4 is not running under Steam this session
	const auto steamUser = reinterpret_cast<AccessorFn>(GetProcAddress(module, "SteamUser"));
	const auto steamFriends = reinterpret_cast<AccessorFn>(GetProcAddress(module, "SteamFriends"));
	const auto registerCallback = reinterpret_cast<RegisterCallbackFn>(GetProcAddress(module, "SteamAPI_RegisterCallback"));
	const auto unregisterCallback = reinterpret_cast<UnregisterCallbackFn>(GetProcAddress(module, "SteamAPI_UnregisterCallback"));
	if (steamUser == nullptr || steamFriends == nullptr || registerCallback == nullptr || unregisterCallback == nullptr) return false;
	const auto user = reinterpret_cast<ISteamUser017*>(steamUser());
	const auto friends = reinterpret_cast<ISteamFriends014*>(steamFriends());
	if (!AttachWithInterfaces(user, friends)) return false;
	// Registered exactly once per successful attach; Detach() (including the
	// one AttachWithInterfaces ran above) always unregisters and deletes it
	// first, so there is never more than one outstanding callback object.
	callback_ = new JoinRequestCallback(this);
	registerCallbackFn_ = reinterpret_cast<void*>(registerCallback);
	unregisterCallbackFn_ = reinterpret_cast<void*>(unregisterCallback);
	registerCallback(callback_, callback_->GetICallback());
	return true;
}

} // namespace steam
} // namespace sf4e
