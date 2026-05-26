#include "github_release_client.hxx"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <windows.h>
#include <pathcch.h>
#include <shellapi.h>
#include <shlobj.h>
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
				AppendUpdateLog("apply-update.ps1 missing in install dir");
				return false;
			}

			wchar_t params[4096] = { 0 };
			swprintf_s(
				params,
				L"-NoProfile -ExecutionPolicy Bypass -File \"%s\" -InstallDir \"%s\" -StagingDir \"%s\" -WaitPid %lu",
				scriptPath,
				installDir,
				stagingDir,
				waitPid
			);

			char paramsUtf8[4096] = { 0 };
			WidePathToUtf8(params, paramsUtf8, sizeof(paramsUtf8));
			AppendUpdateLog(("spawn apply-update: powershell.exe " + std::string(paramsUtf8)).c_str());

			SHELLEXECUTEINFOW sei = { 0 };
			sei.cbSize = sizeof(sei);
			sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE | SEE_MASK_FLAG_NO_UI;
			sei.lpVerb = L"open";
			sei.lpFile = L"powershell.exe";
			sei.lpParameters = params;
			sei.nShow = SW_HIDE;
			if (!ShellExecuteExW(&sei)) {
				char buf[128] = { 0 };
				snprintf(buf, sizeof(buf), "spawn apply-update failed (Win32 %lu)", GetLastError());
				AppendUpdateLog(buf);
				return false;
			}
			if (sei.hProcess) {
				CloseHandle(sei.hProcess);
			}
			AppendUpdateLog("spawn apply-update ok");
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
			swprintf_s(folderName, L"sf4e-update-%ls", tempSub);
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
			if (FAILED(PathCchCombine(logPath, MAX_PATH, tempDir, L"sf4e-update.log"))) {
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

		static bool DownloadViaPowerShell(const char* url, const wchar_t* destPath, std::string& outError) {
			if (!url || !url[0] || !destPath) {
				outError = "missing URL";
				return false;
			}

			wchar_t urlWide[2048] = { 0 };
			MultiByteToWideChar(CP_UTF8, 0, url, -1, urlWide, 2048);

			wchar_t ps[8192] = { 0 };
			swprintf_s(
				ps,
				L"New-Item -ItemType Directory -Force -Path (Split-Path -Parent '%s') | Out-Null; "
				L"[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; "
				L"Invoke-WebRequest -Uri \"%s\" -OutFile \"%s\" -UserAgent \"sf4e-updater/1.0\" -UseBasicParsing -MaximumRedirection 10 | Out-Null",
				destPath,
				urlWide,
				destPath
			);
			if (!RunPowerShellCommand(ps)) {
				outError = "PowerShell download failed";
				AppendUpdateLog("powershell failed");
				return false;
			}

			WIN32_FILE_ATTRIBUTE_DATA info = { 0 };
			if (!GetFileAttributesExW(destPath, GetFileExInfoStandard, &info)) {
				outError = "PowerShell wrote no file";
				AppendUpdateLog("powershell wrote no file");
				return false;
			}
			ULONGLONG size = ((ULONGLONG)info.nFileSizeHigh << 32) | info.nFileSizeLow;
			if (size == 0) {
				outError = "PowerShell wrote empty file";
				AppendUpdateLog("powershell wrote empty file");
				return false;
			}
			AppendUpdateLog("powershell OK");
			return true;
		}

		static bool DownloadViaCurl(const char* url, const wchar_t* destPath, std::string& outError) {
			if (!url || !url[0] || !destPath) {
				outError = "missing URL";
				return false;
			}

			if (!EnsureParentDirectoryExistsW(destPath, outError)) {
				AppendUpdateLog(("curl mkdir failed: " + outError).c_str());
				return false;
			}

			wchar_t urlWide[2048] = { 0 };
			MultiByteToWideChar(CP_UTF8, 0, url, -1, urlWide, 2048);

			wchar_t cmdLine[4096] = { 0 };
			swprintf_s(
				cmdLine,
				L"curl.exe -fL --retry 2 --connect-timeout 30 --max-time 300 -o \"%s\" \"%s\"",
				destPath,
				urlWide
			);

			STARTUPINFOW si = { 0 };
			PROCESS_INFORMATION pi = { 0 };
			si.cb = sizeof(si);
			if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
				outError = "curl.exe not available";
				AppendUpdateLog("curl not available");
				return false;
			}
			WaitForSingleObject(pi.hProcess, INFINITE);
			DWORD exitCode = 1;
			GetExitCodeProcess(pi.hProcess, &exitCode);
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
			if (exitCode != 0) {
				char buf[64];
				snprintf(buf, sizeof(buf), "curl exit %lu", exitCode);
				outError = buf;
				AppendUpdateLog((std::string("curl failed: ") + outError).c_str());
				return false;
			}

			WIN32_FILE_ATTRIBUTE_DATA info = { 0 };
			if (!GetFileAttributesExW(destPath, GetFileExInfoStandard, &info)) {
				outError = "curl wrote no file";
				AppendUpdateLog("curl wrote no file");
				return false;
			}
			ULONGLONG size = ((ULONGLONG)info.nFileSizeHigh << 32) | info.nFileSizeLow;
			if (size == 0) {
				outError = "curl wrote empty file";
				AppendUpdateLog("curl wrote empty file");
				return false;
			}
			AppendUpdateLog("curl OK");
			return true;
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

			const char* curlUrl = (zipDownloadUrl && zipDownloadUrl[0]) ? zipDownloadUrl : zipApiUrl;
			if (curlUrl && curlUrl[0]) {
				if (DownloadViaCurl(curlUrl, zipPath, attemptError)) {
					return true;
				}
				recordFailure("curl");
			}

			const char* psUrl = (zipDownloadUrl && zipDownloadUrl[0]) ? zipDownloadUrl : zipApiUrl;
			if (psUrl && psUrl[0]) {
				if (DownloadViaPowerShell(psUrl, zipPath, attemptError)) {
					return true;
				}
				recordFailure("powershell");
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
		swprintf_s(zipName, L"sf4e-update-package-%hs.zip", safeTag);
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
				" — details in %TEMP%\\sf4e-update.log";
			return result;
		}

		if (!ExpandZipArchive(zipPath, extractDir)) {
			AppendUpdateLog("extract failed");
			result.error = "Could not extract update package.";
			return result;
		}
		AppendUpdateLog("extract ok");

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
		if (!ValidateStagedPackage(stagingUtf8)) {
			AppendUpdateLog("package validation failed");
			result.error = "Downloaded package failed validation (missing required files).";
			return result;
		}
		AppendUpdateLog("package validation ok");

		if (!SpawnApplyUpdateScript(installDir, stagingDir, GetCurrentProcessId())) {
			result.error = "Could not start apply-update.ps1. Reinstall from a fresh zip.";
			return result;
		}

		AppendUpdateLog("DownloadAndApplyUpdate complete");

		result.ok = true;
		return result;
	}

} // namespace launcher
} // namespace sf4e
