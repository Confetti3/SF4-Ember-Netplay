// Update URL allowlist, version comparison and package tree validation for the release client.
#include "github_release_client_internal.hxx"

namespace sf4e {
namespace launcher {
	namespace detail {

		static const auto& kRequiredPackagePaths = sf4e::package::Required;

		static void ParseVersionTriple(const char* tag, int& major, int& minor, int& patch) {
			major = minor = patch = 0;
			if (!tag || !tag[0]) {
				return;
			}
			const char* p = tag;
			while (*p == 'v' || *p == 'V') {
				p++;
			}
			sscanf_s(p, "%d.%d.%d", &major, &minor, &patch);
		}

		int CompareVersions(const char* a, const char* b) {
			int am = 0, amin = 0, ap = 0, bm = 0, bmin = 0, bp = 0;
			ParseVersionTriple(a, am, amin, ap);
			ParseVersionTriple(b, bm, bmin, bp);
			if (am != bm) {
				return am - bm;
			}
			if (amin != bmin) {
				return amin - bmin;
			}
			return ap - bp;
		}

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

		static bool PathExistsUnderRoot(const wchar_t* baseDir, const wchar_t* relPath) {
			if (!baseDir || !relPath) {
				return false;
			}
			wchar_t path[MAX_PATH * 2] = { 0 };
			if (FAILED(PathCchCombine(path, MAX_PATH * 2, baseDir, relPath))) {
				return false;
			}
			return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
		}

		static bool IsAllowedPackagePath(const wchar_t* path) { return sf4e::package::IsAllowed(path); }

		static bool ValidatePackageTree(const wchar_t* root, const wchar_t* relPrefix) {
			wchar_t dir[MAX_PATH * 2] = { 0 };
			if (relPrefix && relPrefix[0]) {
				if (FAILED(PathCchCombine(dir, MAX_PATH * 2, root, relPrefix))) {
					return false;
				}
			}
			else {
				wcsncpy_s(dir, root, _TRUNCATE);
			}

			wchar_t pattern[MAX_PATH * 2] = { 0 };
			wcsncpy_s(pattern, dir, _TRUNCATE);
			if (FAILED(PathCchAppend(pattern, MAX_PATH * 2, L"*"))) {
				return false;
			}

			WIN32_FIND_DATAW fd = { 0 };
			HANDLE hFind = FindFirstFileW(pattern, &fd);
			if (hFind == INVALID_HANDLE_VALUE) {
				return true;
			}
			do {
				if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
					continue;
				}
				wchar_t rel[MAX_PATH * 2] = { 0 };
				if (relPrefix && relPrefix[0]) {
					if (FAILED(StringCchPrintfW(rel, MAX_PATH * 2, L"%s\\%s", relPrefix, fd.cFileName))) {
						FindClose(hFind);
						return false;
					}
				}
				else if (FAILED(StringCchCopyW(rel, MAX_PATH * 2, fd.cFileName))) {
					FindClose(hFind);
					return false;
				}

				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					if (!ValidatePackageTree(root, rel)) {
						FindClose(hFind);
						return false;
					}
				}
				else if (!IsAllowedPackagePath(rel)) {
					char relUtf8[MAX_PATH * 2] = { 0 };
					WidePathToUtf8(rel, relUtf8, sizeof(relUtf8));
					AppendUpdateLog((std::string("unexpected package file: ") + relUtf8).c_str());
					FindClose(hFind);
					return false;
				}
			} while (FindNextFileW(hFind, &fd));
			FindClose(hFind);
			return true;
		}

		bool ValidateStagedPackage(const wchar_t* stagingDir) {
			for (const wchar_t* rel : kRequiredPackagePaths) {
				if (!PathExistsUnderRoot(stagingDir, rel)) {
					char relUtf8[MAX_PATH * 2] = { 0 };
					WidePathToUtf8(rel, relUtf8, sizeof(relUtf8));
					AppendUpdateLog((std::string("missing package file: ") + relUtf8).c_str());
					return false;
				}
			}
			return ValidatePackageTree(stagingDir, L"");
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
