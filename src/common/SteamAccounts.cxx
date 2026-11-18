#include "SteamAccounts.hxx"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iterator>

#include "SteamKeyValues.hxx"

namespace fs = std::filesystem;

namespace sf4e { namespace steam {
namespace {

// SteamID64 of account 0 of the public universe; a user's folder name is the
// 64-bit id less this.
constexpr std::uint64_t kAccountBase = 76561197960265728ULL;

bool SameFolder(const fs::path& a, const fs::path& b) {
	std::wstring x = a.wstring(), y = b.wstring();
	const auto trim = [](std::wstring& s) { while (!s.empty() && (s.back() == L'\' || s.back() == L'/')) s.pop_back(); };
	trim(x); trim(y);
	return std::equal(x.begin(), x.end(), y.begin(), y.end(), [](wchar_t l, wchar_t r) {
		const auto fold = [](wchar_t c) { return c == L'/' ? L'\' : static_cast<wchar_t>(std::towlower(c)); };
		return fold(l) == fold(r);
	});
}

// A Unix path as Wine shows it; one that already names a drive is left alone.
fs::path WinePath(const std::wstring& path) {
	if (path.size() >= 2 && path[1] == L':') return fs::path(path);
	std::wstring windows = L"Z:";
	for (const wchar_t c : path) windows.push_back(c == L'/' ? L'\' : c);
	return fs::path(windows);
}

// The game's save folder inside one userdata account folder.
fs::path SavesIn(const fs::path& account) {
	return account / L"45760" / L"remote" / L"capcom" / L"superstreetfighteriv" / L"ssf4_savedata";
}

std::string ReadText(const fs::path& path) {
	std::ifstream file(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace

std::vector<fs::path> ClientFolders(const std::wstring& registryPath, const std::wstring& launchingClientPath) {
	std::vector<fs::path> folders;
	const auto add = [&](fs::path folder) {
		if (folder.empty()) return;
		for (const auto& known : folders) if (SameFolder(known, folder)) return;
		folders.push_back(std::move(folder));
	};
	if (!registryPath.empty()) add(fs::path(registryPath));
	if (!launchingClientPath.empty()) add(WinePath(launchingClientPath));
	return folders;
}

std::uint32_t MostRecentAccount(const std::string& loginUsersVdf) {
	// "users" { "<SteamID64>" { ... "MostRecent" "1" ... } ... }
	std::vector<Token> t;
	if (!Tokenize(loginUsersVdf, t) || t.size() < 2 || t[0].kind != Token::Text || t[0].text != "users" || t[1].kind != Token::Open) return 0;
	std::string user;
	int depth = 1;
	for (std::size_t i = 2; i < t.size() && depth > 0;) {
		if (t[i].kind == Token::Close) { --depth; ++i; continue; }
		if (t[i].kind != Token::Text || i + 1 >= t.size()) return 0;
		const std::string& key = t[i].text;
		const Token& value = t[i + 1];
		i += 2;
		if (value.kind == Token::Open) {
			if (depth == 1) user = key;
			++depth;
		} else if (value.kind == Token::Text && depth == 2 && key == "MostRecent" && value.text == "1") {
			// Digits only, and inside the 32-bit account range.
			if (user.empty() || user.size() > 19 || user.find_first_not_of("0123456789") != std::string::npos) return 0;
			const std::uint64_t id = std::stoull(user);
			if (id <= kAccountBase || id - kAccountBase > 0xFFFFFFFFULL) return 0;
			return static_cast<std::uint32_t>(id - kAccountBase);
		} else if (value.kind != Token::Text) return 0;
	}
	return 0;
}

AccountSaves SaveFolders(const std::vector<fs::path>& clients, std::uint32_t registryActive, bool launched) {
	AccountSaves found;
	std::error_code ignored;
	for (const auto& client : clients) {
		const fs::path userdata = client / L"userdata";
		for (fs::directory_iterator at(userdata, ignored), end; !ignored && at != end; at.increment(ignored)) {
			const fs::path saves = SavesIn(at->path());
			if (fs::exists(saves / L"replays-swan.dat", ignored)) found.saves.push_back(saves);
		}
		ignored.clear();
		if (found.saves.empty()) continue;
		const auto has = [&](std::uint32_t account) {
			if (!account) return fs::path();
			const fs::path saves = SavesIn(userdata / std::to_wstring(account));
			return std::find(found.saves.begin(), found.saves.end(), saves) != found.saves.end() ? saves : fs::path();
		};
		found.active = has(registryActive);
		if (found.active.empty() && launched) found.active = has(MostRecentAccount(ReadText(client / L"config" / L"loginusers.vdf")));
		break;
	}
	return found;
}

} } // namespace sf4e::steam
