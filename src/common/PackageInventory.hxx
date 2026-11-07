#pragma once
#include "SelectionAssetPath.hxx"
#include <cwchar>
#include <initializer_list>
// The product's files: SF4 Ember's PackageInventory.inc beside this header,
// or the inventory a product's build names in EMBER_PACKAGE_INVENTORY (a
// quoted path), in the same form.
#ifdef EMBER_PACKAGE_INVENTORY
#define SF4E_PACKAGE_INVENTORY_FILE EMBER_PACKAGE_INVENTORY
#else
#define SF4E_PACKAGE_INVENTORY_FILE "PackageInventory.inc"
#endif
namespace sf4e { namespace package {
static const wchar_t* const Required[] = {
#define SF4E_PACKAGE_REQUIRED(path) L##path,
#define SF4E_PACKAGE_OPTIONAL(path)
#define SF4E_PACKAGE_OBSOLETE(path)
#include SF4E_PACKAGE_INVENTORY_FILE
#undef SF4E_PACKAGE_REQUIRED
#undef SF4E_PACKAGE_OPTIONAL
#undef SF4E_PACKAGE_OBSOLETE
};
static const wchar_t* const Allowed[] = {
#define SF4E_PACKAGE_REQUIRED(path) L##path,
#define SF4E_PACKAGE_OPTIONAL(path) L##path,
#define SF4E_PACKAGE_OBSOLETE(path)
#include SF4E_PACKAGE_INVENTORY_FILE
#undef SF4E_PACKAGE_REQUIRED
#undef SF4E_PACKAGE_OPTIONAL
#undef SF4E_PACKAGE_OBSOLETE
};
// A list, not an array: a product may have no obsolete files.
static const std::initializer_list<const wchar_t*> Obsolete = {
#define SF4E_PACKAGE_REQUIRED(path)
#define SF4E_PACKAGE_OPTIONAL(path)
#define SF4E_PACKAGE_OBSOLETE(path) L##path,
#include SF4E_PACKAGE_INVENTORY_FILE
#undef SF4E_PACKAGE_REQUIRED
#undef SF4E_PACKAGE_OPTIONAL
#undef SF4E_PACKAGE_OBSOLETE
};
// A path this build's inventory names one by one.
inline bool IsListed(const wchar_t* path) {
    if (!path) return false;
    for (const auto* allowed : Allowed) if (_wcsicmp(path, allowed) == 0) return true;
    return false;
}
// Glyph and flag art is also allowed by pattern, so a file a later version adds
// there never makes this build refuse that version's update: assets\input\<name>.png
// or assets\flags\<name>.png|.json, <name> 1-64 of [a-z0-9_-], nothing deeper.
// Like selection art, a file there is Ember's only when a manifest or the list
// above names it; otherwise it is the player's.
inline bool IsArtAssetPath(const wchar_t* path) {
    struct Folder { const wchar_t* root; const wchar_t* extensions[2]; };
    static const Folder folders[] = { {L"assets\\input\\", {L".png", nullptr}}, {L"assets\\flags\\", {L".png", L".json"}} };
    if (!path) return false;
    for (const auto& folder : folders) {
        const size_t prefix = wcslen(folder.root);
        if (wcsncmp(path, folder.root, prefix) != 0) continue;
        const wchar_t* name = path + prefix;
        size_t length = 0;
        for (wchar_t c = name[0]; (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_' || c == L'-'; c = name[++length]) {}
        if (length == 0 || length > 64) return false;
        for (const auto* extension : folder.extensions) if (extension && wcscmp(name + length, extension) == 0) return true;
        return false;
    }
    return false;
}
inline bool IsAllowed(const wchar_t* path) {
    if (!path || !*path || wcsstr(path, L"..") || wcschr(path, L':') || *path == L'\\' || *path == L'/') return false;
    return selection::IsSelectionAssetPath(path) || IsArtAssetPath(path) || IsListed(path);
}
} }
