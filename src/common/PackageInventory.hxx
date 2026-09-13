#pragma once
#include "SelectionAssetPath.hxx"
#include <cwchar>
namespace sf4e { namespace package {
static const wchar_t* const Required[] = {
#define SF4E_PACKAGE_REQUIRED(path) L##path,
#define SF4E_PACKAGE_OPTIONAL(path)
#define SF4E_PACKAGE_OBSOLETE(path)
#include "PackageInventory.inc"
#undef SF4E_PACKAGE_REQUIRED
#undef SF4E_PACKAGE_OPTIONAL
#undef SF4E_PACKAGE_OBSOLETE
};
static const wchar_t* const Allowed[] = {
#define SF4E_PACKAGE_REQUIRED(path) L##path,
#define SF4E_PACKAGE_OPTIONAL(path) L##path,
#define SF4E_PACKAGE_OBSOLETE(path)
#include "PackageInventory.inc"
#undef SF4E_PACKAGE_REQUIRED
#undef SF4E_PACKAGE_OPTIONAL
#undef SF4E_PACKAGE_OBSOLETE
};
static const wchar_t* const Obsolete[] = {
#define SF4E_PACKAGE_REQUIRED(path)
#define SF4E_PACKAGE_OPTIONAL(path)
#define SF4E_PACKAGE_OBSOLETE(path) L##path,
#include "PackageInventory.inc"
#undef SF4E_PACKAGE_REQUIRED
#undef SF4E_PACKAGE_OPTIONAL
#undef SF4E_PACKAGE_OBSOLETE
};
inline bool IsAllowed(const wchar_t* path) {
    if (!path || !*path || wcsstr(path, L"..") || wcschr(path, L':') || *path == L'\\' || *path == L'/') return false;
    if (selection::IsSelectionAssetPath(path)) return true;
    for (const auto* allowed : Allowed) if (_wcsicmp(path, allowed) == 0) return true;
    return false;
}
} }
