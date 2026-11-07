// SF4 Ember's identity with the shell's product rules on
// (Branding::roomFacesArePicks and answersFromInput) and ranks of its own
// (Branding::rankingClasses), for ProductRulesOn. Its Product() links ahead
// of SF4 Ember's (sf4e_product), as another product's does in the UI lab.

#include "../common/ProductIdentity.hxx"
#include "EmbeddedBrand.hxx"

namespace {
const ember::LocaleOverlay NoLocaleOverlays[] = {{nullptr, nullptr}};

// Ranks named by the game, the last from the shared catalog.
constexpr ember::RankingClass Ranks[ember::RankingClassCount] = {
	{"rank_1", "Rank 1", nullptr}, {"rank_2", "Rank 2", nullptr}, {"rank_3", "Rank 3", nullptr},
	{"rank_4", "Rank 4", nullptr}, {"rank_5", "Rank 5", nullptr}, {"rank_6", "Rank 6", nullptr},
	{"rank_7", "Rank 7", nullptr}, {"rank_8", "Rank 8", nullptr}, {"rank_9", "Rank 9", nullptr},
	{"rank_10", nullptr, "class.grand_master"},
};
// The game names band 3 itself; band 4's name has not come yet (empty); the
// rest it never names.
const char* GameRankName(int band) { return band == 3 ? "Game rank 3" : band == 4 ? "" : nullptr; }

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
		Ranks,
		GameRankName,
	},
	L"sf4e-product-rules-test",
	false,
	L"SF4EmberProductRulesTest",
	L"Local\\Ember.ProductRulesTest.",
	L"Local\\sf4e-product-rules-test-",
	L".sf4e-product-rules-test-tmp",
	"ember-product-rules-test",
	"usf4",
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
