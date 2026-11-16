// SF4 Ember's identity, constant-initialized so a crash handler or a static
// initializer can read it. Every value is the literal it replaced; the product
// identity test pins them.

#include "../common/ProductIdentity.hxx"
#include "EmbeddedBrand.hxx"

namespace {
const ember::LocaleOverlay NoLocaleOverlays[] = {{nullptr, nullptr}};

// In the order Help & About lists them and Home's header draws them. Ko-fi has no logo in the
// icon set, so its mark is the cup.
constexpr ember::Link Sf4Links[] = {
	{"discord", "discord.gg/uPNqF5A5uq", ember::LinkMark::Discord, "about.discord", "about.discord_detail"},
	{"kofi", "ko-fi.com/confetti3", ember::LinkMark::Coffee, "about.kofi", "about.kofi_detail"},
	{"website", "embernetplay.link", ember::LinkMark::Ember, "about.website", "about.website_detail"},
	{"twitter", "x.com/kabiefox", ember::LinkMark::Twitter, "about.twitter", "about.twitter_detail"},
	{nullptr, nullptr, ember::LinkMark::Ember, nullptr, nullptr},
};

constexpr ember::ProductIdentity Sf4Ember = {
	{
		"SF4 Ember Netplay",
		"SF4 Ember",
		"Ultra Street Fighter IV",
		0xFF8738,
		sf4e::ui::brand::Pixels,
		sf4e::ui::brand::Size,
		L"brand/ember-menu-background.png",
		&ember::EmberShellTheme,
	},
	L"sf4e",
	false,
	L"SF4Ember",
	L"Local\\Ember.Launcher.",
	L"Local\\sf4e-video-",
	L".sf4e-video-tmp",
	"ember",
	"usf4",
	"https://embernetplay.link/j#",
	"Confetti3/SF4-Ember-Netplay",
	"Confetti3/SF4-Ember-Netplay-Nightly",
	"sf4-ember-netplay-",
	L"sf4-netplay-update-",
	L"sf4e-updater-",
	L"sf4-netplay-update.log",
	L"Launcher.exe",
	L"Updater.exe",
	L"Sidecar.dll",
	L"SSFIV.exe",
	{"45760", L"Super Street Fighter IV - Arcade Edition", L""},
	L"ember-discord.exe",
	L"sf4e.log",
	L"launcher.log",
	L"sf4e-crash.log",
	L"sf4e-crash-",
	NoLocaleOverlays,
	"*.emberreplay;*.usf4replay;*.emberpack",
	nullptr,
	Sf4Links,
};
}

namespace ember {
const ProductIdentity& Product() { return Sf4Ember; }
}
