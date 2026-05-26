#include "github_release_client.hxx"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <pathcch.h>
#include <tlhelp32.h>

#include <nlohmann/json.hpp>

#include "../common/sf4e__NetUtil.hxx"

namespace sf4e {
namespace launcher {

	namespace {

		static const char* kDefaultGithubRepo = "Confetti3/SF4e";

		static const char* kRequiredPackagePaths[] = {
			"Launcher.exe",
			"Sidecar.dll",
			"RelayHost.exe",
			"WebView2Loader.dll",
			"launcher-ui\\index.html",
			"launcher-ui\\app.js",
			"launcher-ui\\styles.css",
			"BUILD_INFO.txt",
			"apply-update.ps1",
			"spdlog.dll",
			"fmt.dll",
			"GameNetworkingSockets.dll",
			"GGPO.dll",
			"libcrypto-3.dll",
			"libprotobuf.dll",
			"abseil_dll.dll",
		};

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

		static bool ValidateStagedPackage(const char* stagingDirUtf8) {
			for (const char* rel : kRequiredPackagePaths) {
				if (!PathExistsUtf8(stagingDirUtf8, rel)) {
					return false;
				}
			}
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
			if (FAILED(PathCchCombine(sidecarPath, MAX_PATH, searchRoot, L"Sidecar.dll"))) {
				return false;
			}
			if (GetFileAttributesW(launcherPath) != INVALID_FILE_ATTRIBUTES &&
				GetFileAttributesW(sidecarPath) != INVALID_FILE_ATTRIBUTES) {
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

		static bool RunPowerShellCommand(const wchar_t* psCommand) {
			wchar_t cmdLine[4096] = { 0 };
			swprintf_s(cmdLine, L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"%s\"", psCommand);
			STARTUPINFOW si = { 0 };
			PROCESS_INFORMATION pi = { 0 };
			si.cb = sizeof(si);
			if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
				return false;
			}
			WaitForSingleObject(pi.hProcess, INFINITE);
			DWORD exitCode = 1;
			GetExitCodeProcess(pi.hProcess, &exitCode);
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			return exitCode == 0;
		}

		static bool ExpandZipArchive(const wchar_t* zipPath, const wchar_t* destDir) {
			wchar_t ps[2048] = { 0 };
			swprintf_s(
				ps,
				L"Expand-Archive -LiteralPath '%s' -DestinationPath '%s' -Force",
				zipPath,
				destDir
			);
			return RunPowerShellCommand(ps);
		}

		static bool SpawnApplyUpdateScript(const wchar_t* installDir, const wchar_t* stagingDir, DWORD waitPid) {
			wchar_t scriptPath[MAX_PATH] = { 0 };
			if (FAILED(PathCchCombine(scriptPath, MAX_PATH, installDir, L"apply-update.ps1"))) {
				return false;
			}
			if (GetFileAttributesW(scriptPath) == INVALID_FILE_ATTRIBUTES) {
				return false;
			}

			wchar_t cmdLine[4096] = { 0 };
			swprintf_s(
				cmdLine,
				L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"%s\" -InstallDir \"%s\" -StagingDir \"%s\" -WaitPid %lu",
				scriptPath,
				installDir,
				stagingDir,
				waitPid
			);

			STARTUPINFOW si = { 0 };
			PROCESS_INFORMATION pi = { 0 };
			si.cb = sizeof(si);
			if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, DETACHED_PROCESS | CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
				return false;
			}
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
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

		static std::string HttpDownloadErrorMessage(const HttpRequestResult& httpResult) {
			switch (httpResult.error) {
			case HttpErrorKind::HttpStatus:
				if (httpResult.statusCode > 0) {
					char buf[64];
					snprintf(buf, sizeof(buf), "HTTP %d", httpResult.statusCode);
					return buf;
				}
				return "HTTP error";
			case HttpErrorKind::Timeout:
				return "timed out";
			case HttpErrorKind::ConnectFailed:
				return "could not connect";
			case HttpErrorKind::ReceiveFailed:
				return "transfer interrupted";
			case HttpErrorKind::EmptyBody:
				return "empty response";
			default:
				return "network error";
			}
		}

		static bool TryHttpDownload(
			const char* url,
			const char* headers,
			const wchar_t* zipPath,
			std::string& outError
		) {
			if (!url || !url[0]) {
				return false;
			}
			HttpRequestResult httpResult;
			if (HttpDownloadUrlUtf8(url, zipPath, 300000, headers, &httpResult)) {
				return true;
			}
			outError = HttpDownloadErrorMessage(httpResult);
			return false;
		}

		static bool DownloadViaPowerShell(const char* url, const wchar_t* destPath) {
			if (!url || !url[0] || !destPath) {
				return false;
			}

			wchar_t urlWide[2048] = { 0 };
			MultiByteToWideChar(CP_UTF8, 0, url, -1, urlWide, 2048);

			wchar_t ps[8192] = { 0 };
			swprintf_s(
				ps,
				L"try { Invoke-WebRequest -Uri \"%s\" -OutFile \"%s\" -UserAgent \"sf4e-updater/1.0\" -UseBasicParsing | Out-Null; exit 0 } catch { exit 1 }",
				urlWide,
				destPath
			);
			if (!RunPowerShellCommand(ps)) {
				return false;
			}

			WIN32_FILE_ATTRIBUTE_DATA info = { 0 };
			if (!GetFileAttributesExW(destPath, GetFileExInfoStandard, &info)) {
				return false;
			}
			ULONGLONG size = ((ULONGLONG)info.nFileSizeHigh << 32) | info.nFileSizeLow;
			return size > 0;
		}

		static bool DownloadReleaseZip(
			const char* zipApiUrl,
			const char* zipDownloadUrl,
			const wchar_t* zipPath,
			std::string& outError
		) {
			const char* apiHeaders = "Accept: application/octet-stream\r\nUser-Agent: sf4e-updater/1.0\r\n";
			const char* browserHeaders = "User-Agent: sf4e-updater/1.0\r\n";

			if (zipApiUrl && zipApiUrl[0] && TryHttpDownload(zipApiUrl, apiHeaders, zipPath, outError)) {
				return true;
			}
			DeleteFileW(zipPath);

			if (zipDownloadUrl && zipDownloadUrl[0] && TryHttpDownload(zipDownloadUrl, browserHeaders, zipPath, outError)) {
				return true;
			}
			DeleteFileW(zipPath);

			const char* psUrl = (zipDownloadUrl && zipDownloadUrl[0]) ? zipDownloadUrl : zipApiUrl;
			if (psUrl && psUrl[0] && DownloadViaPowerShell(psUrl, zipPath)) {
				return true;
			}
			DeleteFileW(zipPath);

			if (outError.empty()) {
				outError = "all download methods failed";
			}
			return false;
		}

	} // namespace

	bool GetLauncherInstallDir(wchar_t* outDir, int outDirChars) {
		if (!outDir || outDirChars <= 0) {
			return false;
		}
		if (GetModuleFileNameW(NULL, outDir, outDirChars) == 0) {
			return false;
		}
		return SUCCEEDED(PathCchRemoveFileSpec(outDir, outDirChars));
	}

	bool ReadInstalledVersion(char* outVersion, int outVersionLen) {
		if (!outVersion || outVersionLen <= 0) {
			return false;
		}
		strncpy_s(outVersion, outVersionLen, "unknown", _TRUNCATE);

		wchar_t installDir[MAX_PATH] = { 0 };
		if (!GetLauncherInstallDir(installDir, MAX_PATH)) {
			return false;
		}

		wchar_t infoPath[MAX_PATH] = { 0 };
		if (FAILED(PathCchCombine(infoPath, MAX_PATH, installDir, L"BUILD_INFO.txt"))) {
			return false;
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
				for (const auto& asset : release["assets"]) {
					std::string name = asset.value("name", "");
					if (name.find(".zip") != std::string::npos && name.find("sf4-enhanced-team") != std::string::npos) {
						result.zipDownloadUrl = asset.value("browser_download_url", "");
						result.zipApiUrl = asset.value("url", "");
						break;
					}
				}
			}

			if (result.zipDownloadUrl.empty()) {
				result.error = "Latest release has no sf4-enhanced-team zip asset.";
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
		const char* latestVersionTag
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
		wchar_t tempSub[128] = { 0 };
		MultiByteToWideChar(CP_UTF8, 0, safeTag, -1, tempSub, 128);
		if (GetTempPathW(MAX_PATH, tempRoot) == 0) {
			result.error = "Could not access temp directory.";
			return result;
		}
		PathCchAppend(tempRoot, MAX_PATH, L"sf4e-update-");
		PathCchAppend(tempRoot, MAX_PATH, tempSub);

		CreateDirectoryW(tempRoot, NULL);

		wchar_t zipPath[MAX_PATH] = { 0 };
		wchar_t extractDir[MAX_PATH] = { 0 };
		PathCchCombine(zipPath, MAX_PATH, tempRoot, L"package.zip");
		PathCchCombine(extractDir, MAX_PATH, tempRoot, L"extract");
		CreateDirectoryW(extractDir, NULL);

		std::string downloadError;
		if (!DownloadReleaseZip(zipApiUrl, zipDownloadUrl, zipPath, downloadError)) {
			result.error = "Download failed (" + downloadError + "). Check your connection or download the zip manually from GitHub Releases.";
			return result;
		}

		if (!ExpandZipArchive(zipPath, extractDir)) {
			result.error = "Could not extract update package.";
			return result;
		}

		wchar_t stagingDir[MAX_PATH] = { 0 };
		if (!FindPackageRoot(extractDir, stagingDir, MAX_PATH)) {
			result.error = "Downloaded package is missing Launcher.exe or Sidecar.dll.";
			return result;
		}

		char stagingUtf8[MAX_PATH * 2] = { 0 };
		if (!WideToUtf8(stagingDir, stagingUtf8, sizeof(stagingUtf8))) {
			result.error = "Could not read staging path.";
			return result;
		}
		if (!ValidateStagedPackage(stagingUtf8)) {
			result.error = "Downloaded package failed validation (missing required files).";
			return result;
		}

		if (!SpawnApplyUpdateScript(installDir, stagingDir, GetCurrentProcessId())) {
			result.error = "Could not start apply-update.ps1. Reinstall from a fresh zip.";
			return result;
		}

		result.ok = true;
		return result;
	}

} // namespace launcher
} // namespace sf4e
