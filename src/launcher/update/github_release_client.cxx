#include "github_release_client.hxx"

#include "../../common/install_paths.hxx"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include <windows.h>
#include <bcrypt.h>
#include <pathcch.h>
#include <shellapi.h>
#include <strsafe.h>
#include <shlobj.h>
#include <tlhelp32.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#pragma comment(lib, "bcrypt.lib")

#include <nlohmann/json.hpp>

#include "../common/sf4e__NetUtil.hxx"

namespace sf4e {
namespace launcher {

	namespace {

		static const char* kDefaultGithubRepo = "Confetti3/SF4-Netplay-Launcher";

		static const wchar_t* kRequiredPackagePaths[] = {
			L"Launcher.exe",
			L"Sidecar.dll",
			L"RelayHost.exe",
			L"Updater.exe",
			L"spdlog.dll",
			L"fmt.dll",
			L"GameNetworkingSockets.dll",
			L"GGPO.dll",
			L"libcrypto-3.dll",
			L"libprotobuf.dll",
			L"abseil_dll.dll",
			L"Qt6Core.dll",
			L"Qt6Gui.dll",
			L"Qt6Widgets.dll",
			L"qt.conf",
			L"plugins\\platforms\\qwindows.dll",
			L"preflight.ps1",
			L"preflight.cmd",
			L"START_HERE.md",
			L"BUILD_INFO.txt",
		};

		static const wchar_t* kAllowedPackagePaths[] = {
			L"Launcher.exe",
			L"Sidecar.dll",
			L"RelayHost.exe",
			L"Updater.exe",
			L"START_HERE.md",
			L"preflight.cmd",
			L"preflight.ps1",
			L"qt.conf",
			L"MANIFEST.txt",
			L"ATTRIBUTION.md",
			L"BUILD_INFO.txt",
			L"spdlog.dll",
			L"fmt.dll",
			L"GameNetworkingSockets.dll",
			L"GGPO.dll",
			L"libcrypto-3.dll",
			L"libprotobuf.dll",
			L"abseil_dll.dll",
			L"Qt6Core.dll",
			L"Qt6Gui.dll",
			L"Qt6Widgets.dll",
			L"Qt6Network.dll",
			L"icudt78.dll",
			L"icuin78.dll",
			L"icuuc78.dll",
			L"double-conversion.dll",
			L"pcre2-16.dll",
			L"md4c.dll",
			L"zlib1.dll",
			L"plugins\\generic\\qtuiotouchplugin.dll",
			L"plugins\\imageformats\\qgif.dll",
			L"plugins\\imageformats\\qico.dll",
			L"plugins\\imageformats\\qjpeg.dll",
			L"plugins\\networkinformation\\qnetworklistmanager.dll",
			L"plugins\\platforms\\qwindows.dll",
			L"plugins\\styles\\qmodernwindowsstyle.dll",
			L"plugins\\tls\\qcertonlybackend.dll",
			L"plugins\\tls\\qschannelbackend.dll",
			L"docs\\TROUBLESHOOTING.md",
			L"docs\\USER_NETPLAY.md",
			L"docs\\BETA_TESTERS.md",
			L"docs\\SCOPE_AND_LIMITATIONS.md",
			L"docs\\TEAM_QUICKSTART.md",
			L"docs\\SMOKE_TEST.md",
			L"docs\\NETPLAY_INVARIANTS.md",
			L"docs\\WINDOWS_DEFENDER.md",
			L"docs\\CODE_SIGNING.md",
		};

		static void AppendUpdateLog(const char* message);
		static bool WidePathToUtf8(const wchar_t* wide, char* out, int outLen);

		static void GetGithubRepo(char* outRepo, int outRepoLen) {
			const char* env = getenv("SF4E_GITHUB_REPO");
			if (env && env[0]) {
				strncpy_s(outRepo, outRepoLen, env, _TRUNCATE);
				return;
			}
			strncpy_s(outRepo, outRepoLen, kDefaultGithubRepo, _TRUNCATE);
		}

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

		static int CompareVersions(const char* a, const char* b) {
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

		static bool PathExistsUtf8(const char* baseUtf8, const char* relUtf8) {
			if (!baseUtf8 || !relUtf8) {
				return false;
			}
			char path[MAX_PATH * 2] = { 0 };
			snprintf(path, sizeof(path), "%s\\%s", baseUtf8, relUtf8);
			wchar_t wPath[MAX_PATH * 2] = { 0 };
			MultiByteToWideChar(CP_UTF8, 0, path, -1, wPath, MAX_PATH * 2);
			return GetFileAttributesW(wPath) != INVALID_FILE_ATTRIBUTES;
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

		static bool IsAllowedUpdateUrl(const char* url) {
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

		static bool IsAllowedPackagePath(const wchar_t* relPath) {
			if (!relPath || !relPath[0]) {
				return false;
			}
			if (_wcsicmp(relPath, L"SECURITY.md") == 0) {
				return true;
			}
			if (_wcsnicmp(relPath, L"plugins\\", 8) == 0) {
				const wchar_t* ext = wcsrchr(relPath, L'.');
				if (ext && (_wcsicmp(ext, L".dll") == 0 || _wcsicmp(ext, L".pdb") == 0)) {
					return true;
				}
			}
			for (const wchar_t* allowed : kAllowedPackagePaths) {
				if (_wcsicmp(relPath, allowed) == 0) {
					return true;
				}
			}
			return false;
		}

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

		static bool ValidateStagedPackage(const wchar_t* stagingDir) {
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

		static bool ValidateExtractedTree(const wchar_t* extractRoot) {
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

		static bool FindPackageRoot(const wchar_t* searchRoot, wchar_t* outRoot, int outRootChars) {
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

		static bool RunProcessAndWaitHidden(const wchar_t* cmdLine, DWORD* outExitCode) {
			STARTUPINFOW si = { 0 };
			PROCESS_INFORMATION pi = { 0 };
			si.cb = sizeof(si);
			wchar_t mutableCmd[4096] = { 0 };
			wcsncpy_s(mutableCmd, cmdLine, _TRUNCATE);
			if (!CreateProcessW(NULL, mutableCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
				return false;
			}
			WaitForSingleObject(pi.hProcess, INFINITE);
			DWORD exitCode = 1;
			GetExitCodeProcess(pi.hProcess, &exitCode);
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			if (outExitCode) {
				*outExitCode = exitCode;
			}
			return true;
		}

		// Streaming SHA-256 of a file, returned as lowercase hex. Used to verify a
		// downloaded release zip against the digest GitHub publishes for the asset,
		// so a tampered or corrupted download is rejected before we extract and run
		// any of its contents.
		static bool ComputeFileSha256Hex(const wchar_t* filePath, std::string& outHex) {
			outHex.clear();

			BCRYPT_ALG_HANDLE hAlg = NULL;
			if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0))) {
				return false;
			}

			DWORD cbHashObject = 0;
			DWORD cbData = 0;
			DWORD cbHash = 0;
			bool ok = NT_SUCCESS(BCryptGetProperty(
						  hAlg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&cbHashObject, sizeof(DWORD), &cbData, 0)) &&
				NT_SUCCESS(BCryptGetProperty(
					hAlg, BCRYPT_HASH_LENGTH, (PUCHAR)&cbHash, sizeof(DWORD), &cbData, 0));

			PUCHAR pbHashObject = ok ? (PUCHAR)HeapAlloc(GetProcessHeap(), 0, cbHashObject) : NULL;
			PUCHAR pbHash = ok ? (PUCHAR)HeapAlloc(GetProcessHeap(), 0, cbHash) : NULL;
			BCRYPT_HASH_HANDLE hHash = NULL;
			HANDLE hFile = INVALID_HANDLE_VALUE;
			if (!pbHashObject || !pbHash) {
				ok = false;
			}

			if (ok && !NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, pbHashObject, cbHashObject, NULL, 0, 0))) {
				ok = false;
			}

			if (ok) {
				hFile = CreateFileW(
					filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
					FILE_FLAG_SEQUENTIAL_SCAN, NULL);
				if (hFile == INVALID_HANDLE_VALUE) {
					ok = false;
				}
			}

			if (ok) {
				BYTE buffer[8192];
				DWORD cbRead = 0;
				for (;;) {
					if (!ReadFile(hFile, buffer, sizeof(buffer), &cbRead, NULL)) {
						ok = false;
						break;
					}
					if (cbRead == 0) {
						break;
					}
					if (!NT_SUCCESS(BCryptHashData(hHash, buffer, cbRead, 0))) {
						ok = false;
						break;
					}
				}
			}

			if (ok && !NT_SUCCESS(BCryptFinishHash(hHash, pbHash, cbHash, 0))) {
				ok = false;
			}

			if (ok) {
				char pair[3];
				for (DWORD i = 0; i < cbHash; i++) {
					snprintf(pair, sizeof(pair), "%02x", pbHash[i]);
					outHex += pair;
				}
			}

			if (hFile != INVALID_HANDLE_VALUE) {
				CloseHandle(hFile);
			}
			if (hHash) {
				BCryptDestroyHash(hHash);
			}
			if (pbHashObject) {
				HeapFree(GetProcessHeap(), 0, pbHashObject);
			}
			if (pbHash) {
				HeapFree(GetProcessHeap(), 0, pbHash);
			}
			BCryptCloseAlgorithmProvider(hAlg, 0);
			return ok;
		}

		// Case-insensitive equality for hex digests.
		static bool HexEqualsIgnoreCase(const std::string& a, const std::string& b) {
			if (a.size() != b.size()) {
				return false;
			}
			for (size_t i = 0; i < a.size(); i++) {
				if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
					return false;
				}
			}
			return true;
		}

		static bool ExpandZipArchive(const wchar_t* zipPath, const wchar_t* destDir) {
			wchar_t cmdLine[4096] = { 0 };
			swprintf_s(cmdLine, L"tar.exe -xf \"%s\" -C \"%s\"", zipPath, destDir);

			char cmdUtf8[4096] = { 0 };
			WidePathToUtf8(cmdLine, cmdUtf8, sizeof(cmdUtf8));
			AppendUpdateLog(cmdUtf8);

			DWORD exitCode = 1;
			if (!RunProcessAndWaitHidden(cmdLine, &exitCode)) {
				AppendUpdateLog("tar spawn failed");
				return false;
			}
			if (exitCode != 0) {
				char buf[64] = { 0 };
				snprintf(buf, sizeof(buf), "tar failed with exit code %u", (unsigned)exitCode);
				AppendUpdateLog(buf);
				return false;
			}
			return true;
		}

		static bool SpawnUpdater(const wchar_t* installDir, const wchar_t* stagingDir, DWORD waitPid) {
			wchar_t updaterPath[MAX_PATH] = { 0 };
			if (FAILED(PathCchCombine(updaterPath, MAX_PATH, installDir, L"Updater.exe"))) {
				return false;
			}
			if (GetFileAttributesW(updaterPath) == INVALID_FILE_ATTRIBUTES) {
				AppendUpdateLog("Updater.exe missing in install dir");
				return false;
			}

			wchar_t params[4096] = { 0 };
			swprintf_s(
				params,
				L"-InstallDir \"%s\" -StagingDir \"%s\" -WaitPid %lu",
				installDir,
				stagingDir,
				waitPid
			);

			char paramsUtf8[4096] = { 0 };
			WidePathToUtf8(params, paramsUtf8, sizeof(paramsUtf8));
			AppendUpdateLog(("spawn Updater.exe " + std::string(paramsUtf8)).c_str());

			SHELLEXECUTEINFOW sei = { 0 };
			sei.cbSize = sizeof(sei);
			sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE | SEE_MASK_FLAG_NO_UI;
			sei.lpVerb = L"open";
			sei.lpFile = updaterPath;
			sei.lpParameters = params;
			sei.nShow = SW_HIDE;
			if (!ShellExecuteExW(&sei)) {
				char buf[128] = { 0 };
				snprintf(buf, sizeof(buf), "spawn Updater failed (Win32 %lu)", GetLastError());
				AppendUpdateLog(buf);
				return false;
			}
			if (sei.hProcess) {
				CloseHandle(sei.hProcess);
			}
			AppendUpdateLog("spawn Updater ok");
			return true;
		}

		static bool WideToUtf8(const wchar_t* wide, char* out, int outLen) {
			if (!wide || !out || outLen <= 0) {
				return false;
			}
			int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, outLen, NULL, NULL);
			return n > 0;
		}

		static void SanitizeTagForPath(const char* tag, char* out, int outLen) {
			if (!tag || !out || outLen <= 0) {
				return;
			}
			strncpy_s(out, outLen, tag, _TRUNCATE);
			for (char* p = out; *p; p++) {
				if (*p == '\\' || *p == '/' || *p == ':' || *p == '*' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') {
					*p = '_';
				}
			}
		}

		static bool WidePathToUtf8(const wchar_t* wide, char* out, int outLen) {
			if (!wide || !out || outLen <= 0) {
				return false;
			}
			int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, outLen, NULL, NULL);
			return n > 0;
		}

		static bool EnsureDirectoryExistsW(const wchar_t* dir, std::string& outError) {
			if (!dir || !dir[0]) {
				outError = "empty directory path";
				return false;
			}

			DWORD attrs = GetFileAttributesW(dir);
			if (attrs != INVALID_FILE_ATTRIBUTES) {
				if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
					return true;
				}
				outError = "path exists but is not a directory";
				return false;
			}

			const int createResult = SHCreateDirectoryExW(NULL, dir, NULL);
			if (createResult == ERROR_SUCCESS || createResult == ERROR_ALREADY_EXISTS) {
				attrs = GetFileAttributesW(dir);
				if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
					return true;
				}
			}

			char buf[160] = { 0 };
			snprintf(buf, sizeof(buf), "could not create directory (Win32 %d)", createResult);
			outError = buf;
			return false;
		}

		static bool EnsureParentDirectoryExistsW(const wchar_t* filePath, std::string& outError) {
			if (!filePath || !filePath[0]) {
				outError = "empty file path";
				return false;
			}
			wchar_t parent[MAX_PATH] = { 0 };
			wcsncpy_s(parent, filePath, _TRUNCATE);
			if (FAILED(PathCchRemoveFileSpec(parent, MAX_PATH))) {
				outError = "could not resolve parent directory";
				return false;
			}
			return EnsureDirectoryExistsW(parent, outError);
		}

		static bool BuildUpdateTempRoot(
			const char* safeTag,
			wchar_t* tempRoot,
			size_t tempRootChars,
			std::string& outError
		) {
			if (!tempRoot || tempRootChars == 0) {
				outError = "invalid temp buffer";
				return false;
			}
			tempRoot[0] = L'\0';

			wchar_t tempSub[128] = { 0 };
			MultiByteToWideChar(CP_UTF8, 0, safeTag ? safeTag : "", -1, tempSub, 128);

			wchar_t tempBase[MAX_PATH] = { 0 };
			if (GetTempPathW(MAX_PATH, tempBase) == 0) {
				outError = "GetTempPathW failed";
				return false;
			}

			wchar_t folderName[128] = { 0 };
			swprintf_s(folderName, L"sf4-netplay-update-%ls", tempSub);
			if (FAILED(PathCchCombine(tempRoot, tempRootChars, tempBase, folderName))) {
				outError = "PathCchCombine failed";
				return false;
			}
			return true;
		}

		static void AppendUpdateLog(const char* message) {
			if (!message || !message[0]) {
				return;
			}
			wchar_t tempDir[MAX_PATH] = { 0 };
			if (GetTempPathW(MAX_PATH, tempDir) == 0) {
				return;
			}
			wchar_t logPath[MAX_PATH] = { 0 };
			if (FAILED(PathCchCombine(logPath, MAX_PATH, tempDir, L"sf4-netplay-update.log"))) {
				return;
			}

			SYSTEMTIME st = { 0 };
			GetLocalTime(&st);
			char line[1024] = { 0 };
			snprintf(
				line,
				sizeof(line),
				"%04u-%02u-%02u %02u:%02u:%02u %s\r\n",
				st.wYear,
				st.wMonth,
				st.wDay,
				st.wHour,
				st.wMinute,
				st.wSecond,
				message
			);

			HANDLE hFile = CreateFileW(
				logPath,
				FILE_APPEND_DATA,
				FILE_SHARE_READ,
				NULL,
				OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL,
				NULL
			);
			if (hFile == INVALID_HANDLE_VALUE) {
				return;
			}
			DWORD written = 0;
			WriteFile(hFile, line, (DWORD)strlen(line), &written, NULL);
			CloseHandle(hFile);
		}

		static std::string HttpDownloadErrorMessage(const HttpRequestResult& httpResult) {
			const unsigned long win32 = httpResult.win32Error ? httpResult.win32Error : GetLastError();
			char buf[160];
			switch (httpResult.error) {
			case HttpErrorKind::HttpStatus:
				if (httpResult.statusCode > 0) {
					snprintf(buf, sizeof(buf), "HTTP %d", httpResult.statusCode);
					return buf;
				}
				return "HTTP error";
			case HttpErrorKind::Timeout:
				return "timed out";
			case HttpErrorKind::ConnectFailed:
				snprintf(buf, sizeof(buf), "could not connect (Win32 %lu)", win32);
				return buf;
			case HttpErrorKind::ReceiveFailed:
				snprintf(buf, sizeof(buf), "transfer interrupted (Win32 %lu)", win32);
				return buf;
			case HttpErrorKind::EmptyBody:
				return "empty response";
			case HttpErrorKind::SendFailed:
				snprintf(buf, sizeof(buf), "request failed (Win32 %lu)", win32);
				return buf;
			case HttpErrorKind::OpenFailed:
				snprintf(buf, sizeof(buf), "connection open failed (Win32 %lu)", win32);
				return buf;
			case HttpErrorKind::WriteFailed:
				snprintf(buf, sizeof(buf), "could not write temp file (Win32 %lu)", win32);
				return buf;
			case HttpErrorKind::InvalidArgs:
				return "invalid download URL";
			default:
				snprintf(buf, sizeof(buf), "unknown error (kind %d, Win32 %lu)", (int)httpResult.error, win32);
				return buf;
			}
		}

		static bool TryHttpDownload(
			const char* label,
			const char* url,
			const char* headers,
			const wchar_t* zipPath,
			std::string& outError
		) {
			if (!url || !url[0]) {
				outError = "missing URL";
				return false;
			}
			if (!IsAllowedUpdateUrl(url)) {
				outError = "download URL host is not allowlisted";
				AppendUpdateLog((std::string(label) + " blocked: disallowed host").c_str());
				return false;
			}
			if (!EnsureParentDirectoryExistsW(zipPath, outError)) {
				AppendUpdateLog((std::string(label) + " mkdir failed: " + outError).c_str());
				return false;
			}
			HttpRequestResult httpResult;
			if (HttpDownloadUrlUtf8(url, zipPath, 300000, headers, &httpResult)) {
				AppendUpdateLog((std::string(label) + " OK").c_str());
				return true;
			}
			outError = HttpDownloadErrorMessage(httpResult);
			AppendUpdateLog((std::string(label) + " failed: " + outError).c_str());
			return false;
		}

		static bool DownloadReleaseZip(
			const char* zipApiUrl,
			const char* zipDownloadUrl,
			const wchar_t* zipPath,
			std::string& outError
		) {
			const char* apiHeaders = "Accept: application/octet-stream\r\nUser-Agent: sf4e-updater/1.0\r\n";
			const char* browserHeaders = "User-Agent: sf4e-updater/1.0\r\n";
			std::vector<std::string> attempts;
			std::string attemptError;

			auto recordFailure = [&](const char* label) {
				if (!attemptError.empty()) {
					attempts.push_back(std::string(label) + ": " + attemptError);
				}
				DeleteFileW(zipPath);
			};

			AppendUpdateLog("download start");

			if (zipDownloadUrl && zipDownloadUrl[0]) {
				if (TryHttpDownload("browser", zipDownloadUrl, browserHeaders, zipPath, attemptError)) {
					return true;
				}
				recordFailure("browser");
			}

			if (zipApiUrl && zipApiUrl[0]) {
				if (TryHttpDownload("api", zipApiUrl, apiHeaders, zipPath, attemptError)) {
					return true;
				}
				recordFailure("api");
			}

			std::string detail;
			for (size_t i = 0; i < attempts.size(); i++) {
				if (i > 0) {
					detail += "; ";
				}
				detail += attempts[i];
			}
			if (detail.empty()) {
				outError = "all download methods failed";
			}
			else {
				outError = detail;
			}
			AppendUpdateLog(("download failed: " + outError).c_str());
			return false;
		}

	} // namespace

	bool GetLauncherInstallDir(wchar_t* outDir, int outDirChars) {
		if (!outDir || outDirChars <= 0) {
			return false;
		}
		return sf4e::install::GetInstallRoot(outDir, outDirChars);
	}

	bool ReadInstalledVersion(char* outVersion, int outVersionLen) {
		if (!outVersion || outVersionLen <= 0) {
			return false;
		}
#ifdef SF4E_APP_VERSION
		strncpy_s(outVersion, outVersionLen, SF4E_APP_VERSION, _TRUNCATE);
#else
		strncpy_s(outVersion, outVersionLen, "dev", _TRUNCATE);
#endif

		wchar_t installDir[MAX_PATH] = { 0 };
		if (!GetLauncherInstallDir(installDir, MAX_PATH)) {
			return false;
		}

		wchar_t infoPath[MAX_PATH] = { 0 };
		if (FAILED(PathCchCombine(infoPath, MAX_PATH, installDir, L"readme\\BUILD_INFO.txt"))
			|| GetFileAttributesW(infoPath) == INVALID_FILE_ATTRIBUTES) {
			if (FAILED(PathCchCombine(infoPath, MAX_PATH, installDir, L"BUILD_INFO.txt"))) {
				return false;
			}
		}

		HANDLE hFile = CreateFileW(infoPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			return false;
		}

		char buf[4096] = { 0 };
		DWORD read = 0;
		ReadFile(hFile, buf, sizeof(buf) - 1, &read, NULL);
		CloseHandle(hFile);
		buf[read] = '\0';

		const char* prefixes[] = { "Release:", "Label:" };
		char* line = buf;
		while (line && *line) {
			char* next = strchr(line, '\n');
			if (next) {
				*next = '\0';
			}
			while (*line == ' ' || *line == '\t' || *line == '\r') {
				line++;
			}
			for (const char* prefix : prefixes) {
				if (_strnicmp(line, prefix, strlen(prefix)) == 0) {
					const char* val = line + strlen(prefix);
					while (*val == ' ' || *val == '\t') {
						val++;
					}
					strncpy_s(outVersion, outVersionLen, val, _TRUNCATE);
					size_t len = strlen(outVersion);
					while (len > 0 && (outVersion[len - 1] == ' ' || outVersion[len - 1] == '\r')) {
						outVersion[--len] = '\0';
					}
					return true;
				}
			}
			line = next ? next + 1 : NULL;
		}
		return false;
	}

	bool IsGameProcessRunning() {
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snap == INVALID_HANDLE_VALUE) {
			return false;
		}

		PROCESSENTRY32W pe = { 0 };
		pe.dwSize = sizeof(pe);
		bool running = false;
		if (Process32FirstW(snap, &pe)) {
			do {
				if (_wcsicmp(pe.szExeFile, L"SSFIV.exe") == 0) {
					running = true;
					break;
				}
			} while (Process32NextW(snap, &pe));
		}
		CloseHandle(snap);
		return running;
	}

	UpdateCheckResult CheckForUpdate() {
		UpdateCheckResult result;
		char installed[64] = { 0 };
		ReadInstalledVersion(installed, sizeof(installed));
		result.installedVersion = installed;

		char repo[128] = { 0 };
		GetGithubRepo(repo, sizeof(repo));

		char path[256] = { 0 };
		snprintf(path, sizeof(path), "/repos/%s/releases/latest", repo);

		char body[65536] = { 0 };
		const char* headers = "Accept: application/vnd.github+json\r\nUser-Agent: sf4e-updater/1.0\r\n";
		if (!HttpGetUtf8WithHeaders("api.github.com", 443, true, path, 15000, headers, body, sizeof(body))) {
			result.error = "Could not reach GitHub. Check your internet connection and try again.";
			return result;
		}

		try {
			nlohmann::json release = nlohmann::json::parse(body);
			result.latestVersion = release.value("tag_name", "");
			result.releaseNotes = release.value("body", "");
			result.releaseUrl = release.value("html_url", "");

			if (result.releaseNotes.size() > 2000) {
				result.releaseNotes = result.releaseNotes.substr(0, 2000) + "...";
			}

			if (result.latestVersion.empty()) {
				result.error = "GitHub release has no version tag.";
				return result;
			}

			if (release.contains("assets") && release["assets"].is_array()) {
				std::string legacyZipUrl;
				std::string legacyZipApiUrl;
				std::string legacyDigest;
				// GitHub exposes an asset content digest as "sha256:<hex>". Strip
				// the prefix so we store bare lowercase hex (empty for older
				// releases whose assets predate the digest field).
				auto parseSha256Digest = [](const std::string& digest) -> std::string {
					const std::string prefix = "sha256:";
					if (digest.size() > prefix.size() &&
						digest.compare(0, prefix.size(), prefix) == 0) {
						return digest.substr(prefix.size());
					}
					return "";
				};
				for (const auto& asset : release["assets"]) {
					std::string name = asset.value("name", "");
					if (name.find(".zip") == std::string::npos) {
						continue;
					}
					if (name.find("sf4-netplay-launcher") != std::string::npos) {
						result.zipDownloadUrl = asset.value("browser_download_url", "");
						result.zipApiUrl = asset.value("url", "");
						result.expectedSha256 = parseSha256Digest(asset.value("digest", ""));
						break;
					}
					if (name.find("sf4-enhanced-team") != std::string::npos) {
						legacyZipUrl = asset.value("browser_download_url", "");
						legacyZipApiUrl = asset.value("url", "");
						legacyDigest = parseSha256Digest(asset.value("digest", ""));
					}
				}
				if (result.zipDownloadUrl.empty() && !legacyZipUrl.empty()) {
					result.zipDownloadUrl = legacyZipUrl;
					result.zipApiUrl = legacyZipApiUrl;
					result.expectedSha256 = legacyDigest;
				}
			}

			if (result.zipDownloadUrl.empty()) {
				result.error = "Latest release has no sf4-netplay-launcher zip asset.";
				return result;
			}

			result.updateAvailable = CompareVersions(result.latestVersion.c_str(), installed) > 0;
			result.ok = true;
		}
		catch (...) {
			result.error = "Could not parse GitHub release response.";
		}
		return result;
	}

	ApplyUpdateResult DownloadAndApplyUpdate(
		const char* zipDownloadUrl,
		const char* zipApiUrl,
		const char* latestVersionTag,
		const char* expectedSha256
	) {
		ApplyUpdateResult result;
		if ((!zipDownloadUrl || !zipDownloadUrl[0]) && (!zipApiUrl || !zipApiUrl[0])) {
			result.error = "Missing download URL.";
			return result;
		}
		if (!latestVersionTag || !latestVersionTag[0]) {
			result.error = "Missing version tag.";
			return result;
		}
		if (IsGameProcessRunning()) {
			result.error = "Close Ultra Street Fighter IV before installing an update.";
			return result;
		}

		wchar_t installDir[MAX_PATH] = { 0 };
		if (!GetLauncherInstallDir(installDir, MAX_PATH)) {
			result.error = "Could not determine install directory.";
			return result;
		}

		char safeTag[64] = { 0 };
		SanitizeTagForPath(latestVersionTag, safeTag, sizeof(safeTag));

		wchar_t tempRoot[MAX_PATH] = { 0 };
		std::string tempPathError;
		if (!BuildUpdateTempRoot(safeTag, tempRoot, MAX_PATH, tempPathError)) {
			result.error = "Could not build update temp path (" + tempPathError + ").";
			return result;
		}

		char tempRootUtf8[MAX_PATH * 2] = { 0 };
		WidePathToUtf8(tempRoot, tempRootUtf8, sizeof(tempRootUtf8));
		AppendUpdateLog(("update temp dir: " + std::string(tempRootUtf8)).c_str());

		if (!EnsureDirectoryExistsW(tempRoot, tempPathError)) {
			AppendUpdateLog(("update temp mkdir failed: " + tempPathError).c_str());
			result.error = "Could not create update temp folder (" + tempPathError + ").";
			return result;
		}

		wchar_t tempBase[MAX_PATH] = { 0 };
		if (GetTempPathW(MAX_PATH, tempBase) == 0) {
			result.error = "Could not access temp directory.";
			return result;
		}

		wchar_t zipPath[MAX_PATH] = { 0 };
		wchar_t zipName[128] = { 0 };
		swprintf_s(zipName, L"sf4-netplay-update-package-%hs.zip", safeTag);
		if (FAILED(PathCchCombine(zipPath, MAX_PATH, tempBase, zipName))) {
			result.error = "Could not build update zip path.";
			return result;
		}

		char zipPathUtf8[MAX_PATH * 2] = { 0 };
		WidePathToUtf8(zipPath, zipPathUtf8, sizeof(zipPathUtf8));
		AppendUpdateLog(("update zip path: " + std::string(zipPathUtf8)).c_str());

		wchar_t extractDir[MAX_PATH] = { 0 };
		PathCchCombine(extractDir, MAX_PATH, tempRoot, L"extract");
		if (!EnsureDirectoryExistsW(extractDir, tempPathError)) {
			AppendUpdateLog(("update extract mkdir failed: " + tempPathError).c_str());
			result.error = "Could not create update extract folder (" + tempPathError + ").";
			return result;
		}

		std::string downloadError;
		AppendUpdateLog("DownloadAndApplyUpdate start");
		if (!DownloadReleaseZip(zipApiUrl, zipDownloadUrl, zipPath, downloadError)) {
			char repo[128] = { 0 };
			GetGithubRepo(repo, sizeof(repo));
			char releasePage[256] = { 0 };
			snprintf(
				releasePage,
				sizeof(releasePage),
				"https://github.com/%s/releases/tag/%s",
				repo,
				latestVersionTag
			);
			result.error =
				"Download failed (" + downloadError + "). Manual download: " + releasePage +
				" — details in %TEMP%\\sf4-netplay-update.log";
			return result;
		}

		// Verify the download's SHA-256 against the digest GitHub published for the
		// asset before we extract or run anything from it. A mismatch means the zip
		// was tampered with or corrupted in transit, so refuse it. Releases that
		// predate GitHub asset digests provide no expected hash; those can only be
		// verified by filename allowlist downstream, so we log and continue.
		std::string expectedHash = (expectedSha256 && expectedSha256[0]) ? expectedSha256 : "";
		if (!expectedHash.empty()) {
			std::string actualHash;
			if (!ComputeFileSha256Hex(zipPath, actualHash)) {
				AppendUpdateLog("hash computation failed");
				result.error = "Could not verify the update download. Try again.";
				return result;
			}
			if (!HexEqualsIgnoreCase(actualHash, expectedHash)) {
				AppendUpdateLog(("hash mismatch expected=" + expectedHash + " actual=" + actualHash).c_str());
				result.error =
					"The update download failed its integrity check and was not installed. "
					"This can happen if the download was corrupted; try again, or download "
					"the release manually from GitHub.";
				return result;
			}
			AppendUpdateLog("hash verification ok");
		} else {
			AppendUpdateLog("no published digest for asset; skipping hash verification");
		}

		if (!ExpandZipArchive(zipPath, extractDir)) {
			AppendUpdateLog("extract failed");
			result.error = "Could not extract update package.";
			return result;
		}
		AppendUpdateLog("extract ok");

		if (!ValidateExtractedTree(extractDir)) {
			AppendUpdateLog("extract path validation failed");
			result.error = "Downloaded package contains invalid paths.";
			return result;
		}
		AppendUpdateLog("extract path validation ok");

		wchar_t stagingDir[MAX_PATH] = { 0 };
		if (!FindPackageRoot(extractDir, stagingDir, MAX_PATH)) {
			AppendUpdateLog("package root not found after extract");
			result.error = "Downloaded package is missing Launcher.exe or Sidecar.dll.";
			return result;
		}

		char stagingUtf8[MAX_PATH * 2] = { 0 };
		if (!WideToUtf8(stagingDir, stagingUtf8, sizeof(stagingUtf8))) {
			result.error = "Could not read staging path.";
			return result;
		}
		AppendUpdateLog(("staging dir: " + std::string(stagingUtf8)).c_str());
		if (!ValidateStagedPackage(stagingDir)) {
			AppendUpdateLog("package validation failed");
			result.error = "Downloaded package failed validation (missing required files).";
			return result;
		}
		AppendUpdateLog("package validation ok");

		if (!SpawnUpdater(installDir, stagingDir, GetCurrentProcessId())) {
			result.error = "Could not start Updater.exe. Reinstall from a fresh zip.";
			return result;
		}

		AppendUpdateLog("DownloadAndApplyUpdate complete");

		result.ok = true;
		return result;
	}

} // namespace launcher
} // namespace sf4e
