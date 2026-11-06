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
	// set, the left bar and a hairline along the bottom with no box edge.
	bool underlineFocus;
	// Secondary buttons edged in the border tone rather than the accent's.
	bool quietButtons;
	// A screen's title in capitals.
	bool upperTitles;
};

// SF4 Ember's look: each value is the literal the shell drew before products
// had themes.
constexpr ShellTheme EmberShellTheme = {
	0x141312, 0x191715, 0x211E1B, 0x382D24, 0x3A2A1F, 0x59402B, 0x443A31, 0xFFB16F, 0xF3EBDD, 0xB5A99B, 0xFF5C50, 0xFFAAA0, 0xA4CEA0,
	0x393027, 0x4A3525, 0x76502F, 0x76583D, 0x76614F, 0xF3C58B, 0xF1C477,
	0x100F0E, 0x26221E, 0x38332E, 0xF2EBDE, 0x8C857A,
	4.f, 0.f, 0.f,
	0x443A31, false, false, false,
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
};

// A product's own text for a few msgids of one locale's shared catalog: a PO
// catalog layered over the shared one (Localization.cxx).
struct LocaleOverlay {
	const char* tag;              // a locale tag from locales/locales.json
	const char* po;               // PO text, NUL-terminated UTF-8
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
	const wchar_t* discordExe;

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
