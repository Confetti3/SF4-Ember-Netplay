// SF4 Ember's identity (src/product/Sf4Product.cxx) against the literals it
// replaced, byte for byte. A difference here moves SF4 Ember's settings,
// logs, named objects, links or updates: an installed launcher, a running
// game or a browser link would no longer find them.

#include "../common/ProductIdentity.hxx"
#include "../common/JoinLink.hxx"
#include "../common/ReplayLink.hxx"
#include "../common/TournamentLink.hxx"
#include "../platform/HelperPipe.hxx"
#ifdef _WIN32
#include "../platform/GameWindowSection.hxx"
#include "../platform/JoinLinkMailbox.hxx"
#endif

#include <cstdio>
#include <cstring>
#include <string>

#include "test_support.hxx"

static bool Same(const char* value, const char* literal) { return value && std::strcmp(value, literal) == 0; }
static bool Same(const wchar_t* value, const wchar_t* literal) { return value && std::wcscmp(value, literal) == 0; }

static void TestBranding() {
	const ember::Branding& branding = ember::Product().branding;
	CHECK(Same(branding.productName, "SF4 Ember Netplay"));
	CHECK(Same(branding.shortName, "SF4 Ember"));
	CHECK(Same(branding.gameName, "Ultra Street Fighter IV"));
	CHECK(branding.accent == 0xFF8738u);
	CHECK(branding.mark != nullptr && branding.markSize == 128);
	CHECK(Same(branding.menuBackdrop, L"brand/ember-menu-background.png"));
	// The version resources are written from cmake/sf4e_product.cmake.
	CHECK(Same(branding.productName, SF4E_TEST_PRODUCT_NAME));
	CHECK(Same(SF4E_TEST_PRODUCT_COMPANY, "SF4 Ember Netplay Contributors"));
	CHECK(Same(SF4E_TEST_PRODUCT_COPYRIGHT, "MIT License. Unofficial USF4 netplay port."));
}

static void TestNames() {
	const ember::ProductIdentity& product = ember::Product();
	CHECK(Same(product.appDataFolder, L"sf4e"));
	CHECK(!product.settingsBesideModule);
	CHECK(Same(product.objectStem, L"SF4Ember"));
	CHECK(Same(product.launcherInstancePrefix, L"Local\\Ember.Launcher."));
	CHECK(Same(product.videoLinkPrefix, L"Local\\sf4e-video-"));
	CHECK(Same(product.videoTemporarySuffix, L".sf4e-video-tmp"));
	CHECK(Same(product.urlScheme, "ember"));
	CHECK(Same(product.stableRepo, "Confetti3/SF4-Ember-Netplay"));
	CHECK(Same(product.nightlyRepo, "Confetti3/SF4-Ember-Netplay-Nightly"));
	CHECK(Same(product.releaseZipPrefix, "sf4-ember-netplay-"));
	CHECK(Same(product.updateTempPrefix, L"sf4-netplay-update-"));
	CHECK(Same(product.updaterCopyPrefix, L"sf4e-updater-"));
	CHECK(Same(product.updateLog, L"sf4-netplay-update.log"));
	CHECK(Same(product.launcherExe, L"Launcher.exe"));
	CHECK(Same(product.updaterExe, L"Updater.exe"));
	CHECK(Same(product.gameModule, L"Sidecar.dll"));
	CHECK(Same(product.gameExe, L"SSFIV.exe"));
	CHECK(Same(product.discordExe, L"ember-discord.exe"));
	CHECK(Same(product.gameLog, L"sf4e.log"));
	CHECK(Same(product.launcherLog, L"launcher.log"));
	CHECK(Same(product.crashLog, L"sf4e-crash.log"));
	CHECK(Same(product.crashDumpPrefix, L"sf4e-crash-"));
	// SF4 Ember's text is the shared catalogs' own.
	CHECK(product.localeOverlays != nullptr && product.localeOverlays[0].tag == nullptr);
}

// The helper's names, which every product shares (platform/HelperPipe.hxx)
// and SF4 Ember's identity used to hold: the helper (rust/sf4-net/src/ipc.rs)
// accepts only pipes under this prefix.
static void TestHelperNames() {
	CHECK(Same(sf4e::platform::helper_pipe::Prefix, L"\\\\.\\pipe\\sf4-net-"));
	CHECK(sf4e::platform::helper_pipe::HexDigits == 32);
	CHECK(Same(sf4e::platform::helper_pipe::Executable, L"sf4-net.exe"));
	CHECK(Same(sf4e::platform::helper_pipe::Log, L"sf4-net.log"));
}

// The names as they are put together from the identity.
static void TestComposedNames() {
	CHECK(ember::KernelObjectName(L"JoinLink") == L"Local\\SF4EmberJoinLink");
	CHECK(ember::WindowClassName(L"Recovery") == L"SF4EmberRecovery");
	CHECK(ember::WindowClassName(L"UpdaterProgress") == L"SF4EmberUpdaterProgress");
	CHECK(ember::SchemeLink("join/") == "ember://join/");
	CHECK(sf4e::join_link::ParseUri("ember://join/7K3M-0X1R-T9PZ") == "7K3M0X1RT9PZ");
	CHECK(sf4e::replay_link::MakeReplayLink("a.bin") == "ember://replay/open?file=a.bin");
#ifdef _WIN32
	using namespace sf4e::platform;
	CHECK(Same(JoinLinkSectionName(), L"Local\\SF4EmberJoinLink"));
	CHECK(Same(JoinLinkEventName(), L"Local\\SF4EmberJoinLinkReady"));
	CHECK(Same(MatchLinkSectionName(), L"Local\\SF4EmberMatchLink"));
	CHECK(Same(MatchLinkEventName(), L"Local\\SF4EmberMatchLinkReady"));
	CHECK(Same(PublicRoomLinkSectionName(), L"Local\\SF4EmberPublicRoomLink"));
	CHECK(Same(PublicRoomLinkEventName(), L"Local\\SF4EmberPublicRoomLinkReady"));
	CHECK(Same(ConnectLinkSectionName(), L"Local\\SF4EmberConnectLink"));
	CHECK(Same(ConnectLinkEventName(), L"Local\\SF4EmberConnectLinkReady"));
	CHECK(Same(ReplayLinkSectionName(), L"Local\\SF4EmberReplayLink"));
	CHECK(Same(ReplayLinkEventName(), L"Local\\SF4EmberReplayLinkReady"));
	CHECK(Same(GameWindowSectionName(), L"Local\\SF4EmberGameWindow"));
#endif
}

int main() {
	TestBranding();
	TestNames();
	TestHelperNames();
	TestComposedNames();
	std::printf("SF4 Ember's product identity matches its literals\n");
	return 0;
}
