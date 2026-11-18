#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sf4e { namespace steam {

// The Steam folders to look for userdata in, best first: the SteamPath the
// registry gives, then the Steam client that launched this process. Proton
// keeps SteamPath pointing at a stand-in folder inside the prefix, and tells
// the game where the real client is through STEAM_COMPAT_CLIENT_INSTALL_PATH,
// a Unix path that Wine reaches through its Z: drive. Either may be empty;
// a folder named twice is listed once.
std::vector<std::filesystem::path> ClientFolders(const std::wstring& registryPath, const std::wstring& launchingClientPath);

// The account id (the 32-bit one in userdata folder names) that Steam's
// loginusers.vdf marks MostRecent, 0 when none does or the text is not one
// the file's format allows. Steam keeps one account signed in, so this is the
// one a client running now has.
std::uint32_t MostRecentAccount(const std::string& loginUsersVdf);

// userdata account folders found by SaveFolders: every one holding the game's
// replay index, and the signed-in account's of them.
struct AccountSaves {
	std::vector<std::filesystem::path> saves;
	std::filesystem::path active;
};

// The saves under the first of clients whose userdata holds any. active is
// the registry's ActiveUser when it has saves there. When the registry has no
// ActiveUser (0) or that account has none, and a Steam client is known to be
// running (launched is set: Proton's client is running, since it started the
// game), the client's loginusers.vdf names the account instead.
AccountSaves SaveFolders(const std::vector<std::filesystem::path>& clients, std::uint32_t registryActive, bool launched);

} } // namespace sf4e::steam
