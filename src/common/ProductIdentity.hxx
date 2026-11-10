#pragma once

// What makes a build one product (SF4 Ember, SFV Ember) rather than another:
// the names it shows, the folders and named objects it owns, its links and
// its update repositories. Ember's shared code reads them here instead of
// spelling them out, so two products installed side by side never share a
// settings folder or a link mailbox. The helper is shared by every product:
// its names are platform/HelperPipe.hxx's, not a product's.
//
// Each product defines Product() once, in its own source (SF4 Ember's is
// src/product/Sf4Product.cxx), and every executable and test links that
// definition. A constant table, not a value set at startup: crash handlers,
// static initialisers and tests read it before any entry point runs, and a
// binary without a product fails to link instead of running without names.
//
// C++11, no Windows headers: header-only parsers built as C++11 read it.

#include <string>

namespace ember {

// The shell's colours and shapes (ui/Theme.cxx, ui/V2.cxx), each colour
// 0xRRGGBB. The accent is Branding::accent. EmberShellTheme below is SF4
// Ember's; a product with a look of its own defines its own table and points
// Branding::theme at it.
struct ShellTheme {
	// The design parts' roles (v2::Palette): grounds from ink to hover (header
	// is the focused fill), the accent's light tint, text, the LIVE red, the
	// danger tone (Leave room) and the Ready green.
	unsigned ink, frame, panel, button, header, hover, border, accentLight, text, muted, live, danger, ready;
	// The rest of ImGui's style: a field's hovered and active grounds, a
	// pressed button, the scroll bar under the pointer, every other table
	// row, the plot lines, and the Pending tone.
	unsigned frameHover, active, pressed, scrollHover, rowAlt, plot, pending;
	// The dark backing under words over art (a portrait's label, a footer),
	// a portrait's backing, a name pill's fill, and an entry's words that can
	// and cannot be activated.
	unsigned shade, portrait, pill, entryText, entryTextDisabled;
	// The corner radius of panels, cards, buttons and chips, in logical px.
	float corner;
	// Above 0, the angular cut: buttons and chips lose two opposite corners,
	// and the focus bar and the header's accent rule lean, by this share of
	// their height. 0 keeps Ember's square edges.
	float cut;
	// Above 0, a screen's title and Home's name lean forward by this much per
	// unit of height (0.2 is about 11 degrees). 0 is upright.
	float headingSlant;
	// The hairlines that close the header and open the hint bar.
	unsigned rule;
	// The focus marks: the accent's box edge and left bar (Ember's), or, when
	// set, only the full-height left bar on the focused ground: no box edge,
	// and a focused card drops its border-tone edge too.
	bool barFocus;
	// Secondary buttons edged in the border tone rather than the accent's.
	bool quietButtons;
	// A screen's title in capitals.
	bool upperTitles;
	// The post-match prompt covers the game as the room covers the main menu:
	// the shell's own ground behind it, and the prompt a panel on that ground,
	// edged as the room's panels are, so the prompt and the room it gives way
	// to stand on the same ground. Off (Ember's), the WIN screen shows around
	// the prompt through a veil of ink at 55%.
	bool promptOnGround;
};

// SF4 Ember's look: each value is the literal the shell drew before products
// had themes.
constexpr ShellTheme EmberShellTheme = {
	0x141312, 0x191715, 0x211E1B, 0x382D24, 0x3A2A1F, 0x59402B, 0x443A31, 0xFFB16F, 0xF3EBDD, 0xB5A99B, 0xFF5C50, 0xFFAAA0, 0xA4CEA0,
	0x393027, 0x4A3525, 0x76502F, 0x76583D, 0x76614F, 0xF3C58B, 0xF1C477,
	0x100F0E, 0x26221E, 0x38332E, 0xF2EBDE, 0x8C857A,
	4.f, 0.f, 0.f,
	0x443A31, false, false, false,
	false,
};

// Where a game's own battle HUD is, for Ember's match HUD to keep clear of it
// (ui/MatchHud.cxx). Measured in the game's HUD frame: frameWidth by
// frameHeight units, scaled evenly to the largest that fits the screen and
// centred on it. A product without one has USF4's HUD, as SF4 Ember measured
// it (MatchHud.cxx, PlaceSplit).
struct BattleHud {
	struct Box { float x0, y0, x1, y1; };
	float frameWidth, frameHeight;
	// Each side's plate: the game's own label naming the player (P1, P2).
	Box plate[2];
	// The game writes the players' names on its plates itself: the split
	// layout then draws no plates of its own, only each name's link and class
	// marks, as a chip the box's height against its inner end (P1's right,
	// P2's left), never wider than the box.
	bool namesOnPlates;
	Box marks[2];
	// The lowest point of the game's HUD across the top of the frame: a panel
	// anchored at the top goes just below it, its state line under it.
	float topEnd;
	// The highest point of the game's HUD in the bottom corners (its gauges):
	// a panel anchored in a bottom corner goes just above it. The bottom's
	// middle is clear of it to its foot, where Bottom center stays.
	float bottomStart;
	// The game moves its HUD by an option of its own, which the split layout's
	// name offset follows (USF4's). Without one the setting is not offered.
	bool nameOffset;
};

// A public room class (docs/design/SKILL_BADGE.md) as a product draws and
// names it. The bridge rates every game in the same RankingClassCount bands,
// 1 the lowest; a product names and draws each band as its game's own rank.
// `emblem` is the game's own art for it, the entry SelectionArt::RankingEmblem
// loads from the player's install. `name` is how the game spells the rank
// (in English when the game's own name is read at run time, as
// Branding::rankingClassName), or null for a name from the shared catalog
// (`msgid`). A product without a table has USF4's classes (ui/NameMarks.cxx).
constexpr int RankingClassCount = 10;
struct RankingClass {
	const char* emblem;
	const char* name;
	const char* msgid;
};

// What the shell draws as the product's own.
struct Branding {
	const char* productName;      // window titles, About, the version line
	const char* shortName;
	const char* gameName;         // where the shell names the game
	unsigned accent;              // 0xRRGGBB, the theme's accent
	const unsigned char* mark;    // the header mark, markSize x markSize RGBA
	int markSize;
	const wchar_t* menuBackdrop;  // the menu's backdrop art, under the install's assets folder
	const ShellTheme* theme;      // the shell's colours and shapes; none is EmberShellTheme
	const BattleHud* battleHud;   // the game's battle HUD; none is USF4's
	// The room draws a fighter only where its player shared a pick: a seat, a
	// cabinet and a Members row show the picked fighter, else the "?" a missing
	// picture draws. Off (SF4 Ember), the Members rows show each player's
	// profile main and an unpicked seat the main, by design: a main is a
	// choice every SF4 Ember player makes. A product whose players have no
	// main to show turns it on, so no face stands for a fighter nobody chose.
	bool roomFacesArePicks;
	// The post-match prompt shows the opponent's answer as soon as the
	// battle's input carries it (Battle::RematchChoices, RematchDeclines),
	// not only once the room shows their Ready. Off (SF4 Ember), an opponent
	// who chose Rematch over a WIN that goes on in place reads Choosing until
	// both have chosen.
	bool answersFromInput;
	// The game's own ranks for the bands, RankingClassCount of them, band 1
	// first; none is USF4's classes (SF4 Ember). A product's class whose
	// emblem the install does not have is shown as its name in a pill; one of
	// USF4's is shown as nothing until its emblem loads. A product's emblem is
	// drawn at its own shape, as the game draws it; USF4's sit in a square.
	const RankingClass* rankingClasses;
	// The name of a product's rank for a band (1 to RankingClassCount) as the
	// game itself names it in the player's language, read from the game at run
	// time; null, or an empty name while the game has not given it yet, falls
	// back to `rankingClasses`' name. A name it gives stays valid and unchanged
	// for the process's life, and any thread may ask. None for SF4 Ember.
	const char* (*rankingClassName)(int band);
	// The post-match prompt carries on to the room's results once the WIN is
	// over: an answer it is still waiting for, a Back pressed over the WIN (a
	// press left over from skipping its scenes) and a game whose WIN asked
	// nothing all get Rematch on the results screen, with a fresh countdown
	// (ApplicationShell__PostMatch.cxx). Off (SF4 Ember), the prompt ends with
	// the WIN and the room is shown.
	bool rematchOnResults = false;
};

// A product's own text for a few msgids of one locale's shared catalog: a PO
// catalog layered over the shared one (Localization.cxx).
struct LocaleOverlay {
	const char* tag;              // a locale tag from locales/locales.json
	const char* po;               // PO text, NUL-terminated UTF-8
};

// The game as Steam installs it: where the launcher looks for it, and the
// app id the game it starts needs in its environment (SteamAppId and
// SteamGameId), since the launcher, not Steam, starts it.
struct SteamGame {
	// Steam's app id, which also names a library's appmanifest_<appId>.acf.
	const char* appId;
	// The usual install folder under a library's steamapps\common, tried after
	// the folder the manifest names.
	const wchar_t* installDir;
	// The folder that holds gameExe, under the install folder; empty when it
	// is the install folder itself. The launcher starts the game there.
	const wchar_t* exeFolder;
};

struct ProductIdentity {
	Branding branding;

	// Under %APPDATA% (settings, logs, replays) and %LOCALAPPDATA%.
	const wchar_t* appDataFolder;
	// Where the settings folder (SettingsStore::DefaultDirectory: the
	// settings, the session trace, the sent reports) is: appDataFolder under
	// %APPDATA% when false; when true, appDataFolder beside the module that
	// holds the runtime, for a product with no installer that keeps its logs
	// there too (SFV Ember).
	bool settingsBesideModule;
	// Named objects: "Local\\" + objectStem + role for kernel objects (link
	// mailboxes, the game window section), objectStem + role for window classes.
	const wchar_t* objectStem;
	const wchar_t* launcherInstancePrefix;  // + the user's SID and a scope
	const wchar_t* videoLinkPrefix;         // + a process id and a tick count
	// The video export's reservation beside its video: the video's name, a
	// GUID, then this, an extension no replay export uses.
	const wchar_t* videoTemporarySuffix;
	// Links from a browser: urlScheme + "://join/...", "://room/open?..." and so on.
	const char* urlScheme;
	// The game a bridge's public rooms are for (docs/design/PUBLIC_ROOMS.md,
	// "Games"): "usf4" for SF4 Ember, "sfv" for SFV Ember. A bridge lists,
	// creates and admits a room of one game only, so one product's players
	// never see or join the other's rooms. Requests name it only when it is
	// not "usf4", which is what a request that names none means.
	const char* roomGame;
	// A room's short link as this product shows and copies it: the prefix
	// before the code, "https://embernetplay.link/j#" for SF4 Ember and
	// "https://embernetplay.link/v#" for SFV Ember. The site's /j page opens
	// SF4 Ember (ember://join/...) and its /v page SFV Ember
	// (ember-sfv://join/...); the helper reads either form (short_invite.rs).
	const char* roomLinkPrefix;

	// GitHub repositories (owner/name): Stable and Beta, and Nightly.
	const char* stableRepo;
	const char* nightlyRepo;
	const char* releaseZipPrefix;           // a release package's name: + version + ".zip"
	const wchar_t* updateTempPrefix;        // the updater's folders and packages under %TEMP%
	const wchar_t* updaterCopyPrefix;       // the updater's running copy under %TEMP%
	const wchar_t* updateLog;               // under %TEMP%

	// The installed files the launcher, updater and reports look for.
	const wchar_t* launcherExe;
	const wchar_t* updaterExe;
	const wchar_t* gameModule;              // injected into the game
	const wchar_t* gameExe;
	SteamGame steam;
	const wchar_t* discordExe;               // empty: the product has no Discord companion

	// Under appDataFolder\logs.
	const wchar_t* gameLog;
	const wchar_t* launcherLog;
	const wchar_t* crashLog;
	const wchar_t* crashDumpPrefix;         // + a time, a process id and ".dmp"

	// Catalogs layered over the shared ones, ending with {nullptr, nullptr}.
	const LocaleOverlay* localeOverlays;
};

const ProductIdentity& Product();

// The product's shell theme: its own table, or Ember's.
inline const ShellTheme& ProductTheme() {
	return Product().branding.theme ? *Product().branding.theme : EmberShellTheme;
}

// The product's players have a main fighter: one whose room shows only picks
// (Branding::roomFacesArePicks) has none, so the shell neither asks for one
// (Profile's Main character) nor shows one (the profile card).
inline bool PlayersHaveMain() {
	return !Product().branding.roomFacesArePicks;
}

// "Local\\" + objectStem + role.
inline std::wstring KernelObjectName(const wchar_t* role) {
	return std::wstring(L"Local\\") + Product().objectStem + role;
}
inline std::wstring WindowClassName(const wchar_t* role) {
	return std::wstring(Product().objectStem) + role;
}
// urlScheme + "://" + rest.
inline std::string SchemeLink(const char* rest) {
	return std::string(Product().urlScheme) + "://" + rest;
}

} // namespace ember
