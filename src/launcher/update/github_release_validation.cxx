// Update URL allowlist, version comparison and package tree validation for the release client.
#include "github_release_client_internal.hxx"
#include <cctype>
#include <optional>

namespace sf4e {
namespace launcher {
	// Up to nine digits, so the number fits an int without overflow.
	static bool ReadNumber(const char*& p, int& out) {
		const char* start = p;
		while (*p >= '0' && *p <= '9') p++;
		if (p == start || p - start > 9) return false;
		out = 0;
		for (const char* d = start; d < p; d++) out = out * 10 + (*d - '0');
		return true;
	}

	std::optional<Version> ParseVersion(const char* text) {
		if (!text) return std::nullopt;
		const char* p = text;
		if (*p == 'v' || *p == 'V') p++;
		Version version;
		if (!ReadNumber(p, version.major) || *p++ != '.' || !ReadNumber(p, version.minor) || *p++ != '.' || !ReadNumber(p, version.patch)) return std::nullopt;
		if (*p == '\0') return version;
		if (*p++ != '-' || *p == '\0') return std::nullopt;
		version.prerelease = true;
		for (const char* c = p; *c; c++)
			if (!isalnum(static_cast<unsigned char>(*c)) && *c != '.' && *c != '-') return std::nullopt;
		while (isalpha(static_cast<unsigned char>(*p))) version.word += static_cast<char>(tolower(static_cast<unsigned char>(*p++)));
		if (*p >= '0' && *p <= '9' && !ReadNumber(p, version.number)) return std::nullopt;
		version.rest = p;
		return version;
	}

	int CompareVersions(const Version& a, const Version& b) {
		const auto order = [](auto x, auto y) { return x < y ? -1 : x > y ? 1 : 0; };
		if (int c = order(a.major, b.major)) return c;
		if (int c = order(a.minor, b.minor)) return c;
		if (int c = order(a.patch, b.patch)) return c;
		if (a.prerelease != b.prerelease) return a.prerelease ? -1 : 1;
		if (int c = a.word.compare(b.word)) return c < 0 ? -1 : 1;
		if (int c = order(a.number, b.number)) return c;
		if (int c = a.rest.compare(b.rest)) return c < 0 ? -1 : 1;
		return 0;
	}

	namespace detail {


		static bool ParseHttpsHostFromUrl(const char* url, char* outHost, int outHostLen) {
			if (!url || !outHost || outHostLen <= 0) {
				return false;
			}
			if (_strnicmp(url, "https://", 8) != 0) {
				return false;
			}
			const char* hostStart = url + 8;
			const char* pathStart = strchr(hostStart, '/');
			if (pathStart) {
				strncpy_s(outHost, outHostLen, hostStart, pathStart - hostStart);
			}
			else {
				strncpy_s(outHost, outHostLen, hostStart, _TRUNCATE);
			}
			char* at = strchr(outHost, '@');
			if (at) {
				memmove(outHost, at + 1, strlen(at + 1) + 1);
			}
			char* colon = strchr(outHost, ':');
			if (colon) {
				*colon = 0;
			}
			return outHost[0] != 0;
		}

		static bool IsAllowedUpdateHost(const char* host) {
			if (!host || !host[0]) {
				return false;
			}
			if (_stricmp(host, "api.github.com") == 0) {
				return true;
			}
			if (_stricmp(host, "github.com") == 0 || _stricmp(host, "www.github.com") == 0) {
				return true;
			}
			if (_stricmp(host, "objects.githubusercontent.com") == 0) {
				return true;
			}
			if (_stricmp(host, "codeload.github.com") == 0) {
				return true;
			}
			return false;
		}

		bool IsAllowedUpdateUrl(const char* url) {
			char host[256] = { 0 };
			if (!ParseHttpsHostFromUrl(url, host, sizeof(host))) {
				return false;
			}
			return IsAllowedUpdateHost(host);
		}






		bool ValidateStagedPackage(const wchar_t* stagingDir) {
			std::string error;
			if (launcher::ValidatePackageFolder(stagingDir, error)) return true;
			AppendUpdateLog(("package rejected: " + error).c_str());
			return false;
		}
		static bool IsPathUnderRoot(const wchar_t* root, const wchar_t* candidate) {
			wchar_t rootFull[MAX_PATH * 2] = { 0 };
			wchar_t candidateFull[MAX_PATH * 2] = { 0 };
			if (!GetFullPathNameW(root, MAX_PATH * 2, rootFull, NULL)) {
				return false;
			}
			if (!GetFullPathNameW(candidate, MAX_PATH * 2, candidateFull, NULL)) {
				return false;
			}
			size_t rootLen = wcslen(rootFull);
			if (rootLen > 0 && rootFull[rootLen - 1] != L'\\') {
				wcscat_s(rootFull, L"\\");
				rootLen++;
			}
			if (_wcsnicmp(candidateFull, rootFull, rootLen) != 0) {
				return false;
			}
			return true;
		}
		bool ValidateExtractedTree(const wchar_t* extractRoot) {
			if (!extractRoot || !extractRoot[0]) {
				return false;
			}
			wchar_t pattern[MAX_PATH * 2] = { 0 };
			wcsncpy_s(pattern, extractRoot, _TRUNCATE);
			PathCchAppend(pattern, MAX_PATH * 2, L"*");

			WIN32_FIND_DATAW fd = { 0 };
			HANDLE hFind = FindFirstFileW(pattern, &fd);
			if (hFind == INVALID_HANDLE_VALUE) {
				return false;
			}
			do {
				if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
					continue;
				}
				wchar_t entry[MAX_PATH * 2] = { 0 };
				PathCchCombine(entry, MAX_PATH * 2, extractRoot, fd.cFileName);
				if (!IsPathUnderRoot(extractRoot, entry)) {
					FindClose(hFind);
					return false;
				}
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					if (!ValidateExtractedTree(entry)) {
						FindClose(hFind);
						return false;
					}
				}
			} while (FindNextFileW(hFind, &fd));
			FindClose(hFind);
			return true;
		}

		bool FindPackageRoot(const wchar_t* searchRoot, wchar_t* outRoot, int outRootChars) {
			if (!searchRoot || !outRoot || outRootChars <= 0) {
				return false;
			}

			wchar_t launcherPath[MAX_PATH] = { 0 };
			wchar_t sidecarPath[MAX_PATH] = { 0 };
			if (FAILED(PathCchCombine(launcherPath, MAX_PATH, searchRoot, L"Launcher.exe"))) {
				return false;
			}
			const bool hasLauncher = GetFileAttributesW(launcherPath) != INVALID_FILE_ATTRIBUTES;
			bool hasSidecar = SUCCEEDED(PathCchCombine(sidecarPath, MAX_PATH, searchRoot, L"dll\\Sidecar.dll"))
				&& GetFileAttributesW(sidecarPath) != INVALID_FILE_ATTRIBUTES;
			if (!hasSidecar) {
				hasSidecar = SUCCEEDED(PathCchCombine(sidecarPath, MAX_PATH, searchRoot, L"Sidecar.dll"))
					&& GetFileAttributesW(sidecarPath) != INVALID_FILE_ATTRIBUTES;
			}
			if (hasLauncher && hasSidecar) {
				wcsncpy_s(outRoot, outRootChars, searchRoot, _TRUNCATE);
				return true;
			}

			wchar_t pattern[MAX_PATH] = { 0 };
			wcsncpy_s(pattern, searchRoot, _TRUNCATE);
			PathCchAppend(pattern, MAX_PATH, L"*");

			WIN32_FIND_DATAW fd = { 0 };
			HANDLE hFind = FindFirstFileW(pattern, &fd);
			if (hFind == INVALID_HANDLE_VALUE) {
				return false;
			}
			do {
				if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
					continue;
				}
				if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
					continue;
				}
				wchar_t sub[MAX_PATH] = { 0 };
				PathCchCombine(sub, MAX_PATH, searchRoot, fd.cFileName);
				if (FindPackageRoot(sub, outRoot, outRootChars)) {
					FindClose(hFind);
					return true;
				}
			} while (FindNextFileW(hFind, &fd));
			FindClose(hFind);
			return false;
		}

	} // namespace detail
} // namespace launcher
} // namespace sf4e
