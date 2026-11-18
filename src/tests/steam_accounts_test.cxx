#include "../common/SteamAccounts.hxx"

#include <chrono>
#include <fstream>
#include <string>

#include "test_support.hxx"

namespace fs = std::filesystem;
using namespace sf4e::steam;

// Account 1000 is SteamID64 76561197960266728; 7 is 76561197960265735.
static const std::string kUsers = R"vdf("users"
{
	"76561197960265735"
	{
		"AccountName"		"first"
		"MostRecent"		"0"
	}
	"76561197960266728"
	{
		"AccountName"		"second"
		"PersonaName"		"second"
		"MostRecent"		"1"
	}
}
)vdf";

static fs::path Fresh() {
	const fs::path root = fs::temp_directory_path() / ("sf4e-steam-accounts-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::remove_all(root);
	fs::create_directories(root);
	return root;
}

static fs::path Saves(const fs::path& client, const char* account) {
	const fs::path saves = client / "userdata" / account / "45760" / "remote" / "capcom" / "superstreetfighteriv" / "ssf4_savedata";
	fs::create_directories(saves);
	std::ofstream(saves / "replays-swan.dat") << "x";
	return saves;
}

static void WriteLoginUsers(const fs::path& client, const std::string& text) {
	fs::create_directories(client / "config");
	std::ofstream(client / "config" / "loginusers.vdf", std::ios::binary) << text;
}

static void TestClientFolders() {
	// Windows: the registry's folder only.
	auto folders = ClientFolders(L"C:\\Program Files (x86)\\Steam", L"");
	EXPECT(folders.size() == 1 && folders[0] == fs::path(L"C:\\Program Files (x86)\\Steam"));
	// Proton: the prefix's stand-in folder, then the real client through Z:.
	folders = ClientFolders(L"C:\\Program Files (x86)\\Steam", L"/home/deck/.local/share/Steam");
	EXPECT(folders.size() == 2 && folders[1] == fs::path(L"Z:\\home\\deck\\.local\\share\\Steam"));
	// A client the registry already names is listed once, in either spelling.
	folders = ClientFolders(L"Z:\\home\\deck\\.local\\share\\steam\\\\", L"/home/deck/.local/share/Steam");
	EXPECT(folders.size() == 1);
	// A launching client that names a drive is taken as written.
	folders = ClientFolders(L"", L"D:\\Steam");
	EXPECT(folders.size() == 1 && folders[0] == fs::path(L"D:\\Steam"));
	EXPECT(ClientFolders(L"", L"").empty());
}

static void TestMostRecentAccount() {
	EXPECT(MostRecentAccount(kUsers) == 1000);
	EXPECT(MostRecentAccount("") == 0);
	EXPECT(MostRecentAccount("\"users\"\n{\n}\n") == 0);
	EXPECT(MostRecentAccount("\"other\" { \"76561197960266728\" { \"MostRecent\" \"1\" } }") == 0);
	// None marked, a name that is not a SteamID64, and one outside the
	// 32-bit account range are not accounts.
	EXPECT(MostRecentAccount("\"users\" { \"76561197960266728\" { \"MostRecent\" \"0\" } }") == 0);
	EXPECT(MostRecentAccount("\"users\" { \"alice\" { \"MostRecent\" \"1\" } }") == 0);
	EXPECT(MostRecentAccount("\"users\" { \"76561197960265728\" { \"MostRecent\" \"1\" } }") == 0);
	EXPECT(MostRecentAccount("\"users\" { \"9999999999999999999\" { \"MostRecent\" \"1\" } }") == 0);
	// Cut off text is not read.
	EXPECT(MostRecentAccount("\"users\" { \"76561197960266728\" { \"MostRecent\" ") == 0);
}

static void TestWindowsLookup() {
	const fs::path client = Fresh();
	const fs::path first = Saves(client, "7");
	Saves(client, "1000");
	fs::create_directories(client / "userdata" / "55" / "45760" / "remote");
	WriteLoginUsers(client, kUsers);
	// ActiveUser picks the account; saves are every account holding the index.
	auto found = SaveFolders({client}, 7, false);
	EXPECT(found.saves.size() == 2 && found.active == first);
	// No client running: no active account, whatever loginusers.vdf says.
	found = SaveFolders({client}, 0, false);
	EXPECT(found.saves.size() == 2 && found.active.empty());
	// An ActiveUser without saves is not active.
	found = SaveFolders({client}, 55, false);
	EXPECT(found.active.empty());
	// No userdata at all: nothing.
	found = SaveFolders({client / "missing"}, 7, false);
	EXPECT(found.saves.empty() && found.active.empty());
	fs::remove_all(client);
}

static void TestProtonLookup() {
	const fs::path stub = Fresh(), real = Fresh();
	const fs::path first = Saves(real, "7"), second = Saves(real, "1000");
	WriteLoginUsers(real, kUsers);
	// The prefix's stand-in folder has no userdata, so the real client's is used.
	auto found = SaveFolders({stub, real}, 0, true);
	EXPECT(found.saves.size() == 2);
	// No ActiveUser in the prefix: the client's MostRecent login.
	EXPECT(found.active == second);
	// An ActiveUser that has saves still wins.
	found = SaveFolders({stub, real}, 7, true);
	EXPECT(found.active == first);
	// An ActiveUser without saves falls back to the login file.
	found = SaveFolders({stub, real}, 55, true);
	EXPECT(found.active == second);
	// A MostRecent account without saves leaves none active.
	WriteLoginUsers(real, "\"users\" { \"76561197960265736\" { \"MostRecent\" \"1\" } }");
	found = SaveFolders({stub, real}, 0, true);
	EXPECT(found.saves.size() == 2 && found.active.empty());
	// No login file: none active.
	fs::remove(real / "config" / "loginusers.vdf");
	found = SaveFolders({stub, real}, 0, true);
	EXPECT(found.active.empty());
	// The first client holding saves is the only one read.
	const fs::path other = Fresh();
	Saves(other, "9");
	found = SaveFolders({real, other}, 0, true);
	EXPECT(found.saves.size() == 2);
	fs::remove_all(stub); fs::remove_all(real); fs::remove_all(other);
}

int main() {
	TestClientFolders();
	TestMostRecentAccount();
	TestWindowsLookup();
	TestProtonLookup();
	return TEST_EXIT_CODE();
}
