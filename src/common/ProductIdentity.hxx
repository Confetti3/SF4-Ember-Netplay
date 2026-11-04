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

// What the shell draws as the product's own.
struct Branding {
	const char* productName;      // window titles, About, the version line
	const char* shortName;
	const char* gameName;         // where the shell names the game
	unsigned accent;              // 0xRRGGBB, the theme's accent
	const unsigned char* mark;    // the header mark, markSize x markSize RGBA
	int markSize;
	const wchar_t* menuBackdrop;  // the menu's backdrop art, under the install's assets folder
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
