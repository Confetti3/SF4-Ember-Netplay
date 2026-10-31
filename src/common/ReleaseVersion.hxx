#pragma once
// A release's version as its tag spells it (v1.5.0, 1.5.0-rc2,
// 1.5.0-nightly20261031.2), read the one way the updater and the version
// line both use.
#include "UpdateChannel.hxx"
#include <cctype>
#include <optional>
#include <string>

namespace sf4e { namespace updates {
struct Version {
	int major = 0, minor = 0, patch = 0;
	bool prerelease = false;
	// The pre-release part: its word lowercased ("nightly", "rc"), the number
	// after it and whatever follows.
	std::string word, rest;
	int number = 0;
};
namespace detail {
// Up to nine digits, so the number fits an int without overflow.
inline bool ReadNumber(const char*& p, int& out) {
	const char* start = p;
	while (*p >= '0' && *p <= '9') p++;
	if (p == start || p - start > 9) return false;
	out = 0;
	for (const char* d = start; d < p; d++) out = out * 10 + (*d - '0');
	return true;
}
}
inline std::optional<Version> ParseVersion(const char* text) {
	if (!text) return std::nullopt;
	const char* p = text;
	if (*p == 'v' || *p == 'V') p++;
	Version version;
	if (!detail::ReadNumber(p, version.major) || *p++ != '.' || !detail::ReadNumber(p, version.minor) || *p++ != '.' || !detail::ReadNumber(p, version.patch)) return std::nullopt;
	if (*p == '\0') return version;
	if (*p++ != '-' || *p == '\0') return std::nullopt;
	version.prerelease = true;
	for (const char* c = p; *c; c++)
		if (!std::isalnum(static_cast<unsigned char>(*c)) && *c != '.' && *c != '-') return std::nullopt;
	while (std::isalpha(static_cast<unsigned char>(*p))) version.word += static_cast<char>(std::tolower(static_cast<unsigned char>(*p++)));
	if (*p >= '0' && *p <= '9' && !detail::ReadNumber(p, version.number)) return std::nullopt;
	version.rest = p;
	return version;
}
// No pre-release part: Stable; "nightly": Nightly; any other word: Beta.
inline ReleaseKind ClassifyReleaseKind(const Version& version) {
	if (!version.prerelease) return ReleaseKind::Stable;
	return version.word == "nightly" ? ReleaseKind::Nightly : ReleaseKind::Beta;
}
} }
