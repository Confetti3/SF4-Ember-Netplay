// SF4 Ember's identity, constant-initialized so a crash handler or a static
// initializer can read it. Every value is the literal it replaced; the product
// identity test pins them.

#include "../common/ProductIdentity.hxx"
#include "EmbeddedBrand.hxx"

namespace {
const ember::LocaleOverlay NoLocaleOverlays[] = {{nullptr, nullptr}};

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
	L"ember-discord.exe",
	L"sf4e.log",
	L"launcher.log",
	L"sf4e-crash.log",
	L"sf4e-crash-",
	NoLocaleOverlays,
};
}

namespace ember {
const ProductIdentity& Product() { return Sf4Ember; }
}
