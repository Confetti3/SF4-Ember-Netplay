// SF4 Ember's identity with the shell's product rules on
// (Branding::roomFacesArePicks and answersFromInput), for ProductRulesOn.
// Its Product() links ahead of SF4 Ember's (sf4e_product), as another
// product's does in the UI lab.

#include "../common/ProductIdentity.hxx"
#include "EmbeddedBrand.hxx"

namespace {
const ember::LocaleOverlay NoLocaleOverlays[] = {{nullptr, nullptr}};

constexpr ember::ProductIdentity RulesOn = {
	{
		"SF4 Ember Netplay",
		"SF4 Ember",
		"Ultra Street Fighter IV",
		0xFF8738,
		sf4e::ui::brand::Pixels,
		sf4e::ui::brand::Size,
		L"brand/ember-menu-background.png",
		&ember::EmberShellTheme,
		nullptr,
		true,
		true,
	},
	L"sf4e-product-rules-test",
	false,
	L"SF4EmberProductRulesTest",
	L"Local\\Ember.ProductRulesTest.",
	L"Local\\sf4e-product-rules-test-",
	L".sf4e-product-rules-test-tmp",
	"ember-product-rules-test",
	"",
	"",
	"sf4-ember-product-rules-test-",
	L"sf4-product-rules-test-update-",
	L"sf4e-product-rules-test-updater-",
	L"sf4-product-rules-test-update.log",
	L"",
	L"",
	L"Sidecar.dll",
	L"SSFIV.exe",
	{"45760", L"Super Street Fighter IV - Arcade Edition", L""},
	L"",
	L"sf4e-product-rules-test.log",
	L"launcher.log",
	L"sf4e-product-rules-test-crash.log",
	L"sf4e-product-rules-test-crash-",
	NoLocaleOverlays,
};
}

namespace ember {
const ProductIdentity& Product() { return RulesOn; }
}
