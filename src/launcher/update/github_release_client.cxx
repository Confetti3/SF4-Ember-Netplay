#include "github_release_client_internal.hxx"

namespace sf4e {
namespace launcher {

	namespace detail {

		static void GetGithubRepo(char* outRepo, int outRepoLen) {
			const char* env = getenv("SF4E_GITHUB_REPO");
			if (env && env[0]) {
				strncpy_s(outRepo, outRepoLen, env, _TRUNCATE);
				return;
			}
			strncpy_s(outRepo, outRepoLen, kDefaultGithubRepo, _TRUNCATE);
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

		// The updater must not run from the directory it is about to rewrite:
		// Windows refuses to replace the image of a running executable, so the
		// old in-place launch made its own replacement the expected failure.
		// Copy the staged (new, already validated) updater to a unique directory
		// outside the replacement set and run that. This also means an upgrade
		// started from an older install executes the new updater implementation
		// rather than the one already on disk. Updater.exe imports only system
		// DLLs and the MSVC runtime, so the single file is self-sufficient.
		static bool StageUpdaterOutsideInstall(const wchar_t* installDir, const wchar_t* stagingDir,
			wchar_t* outPath, size_t outLen) {
			wchar_t source[MAX_PATH] = { 0 };
			if (FAILED(PathCchCombine(source, MAX_PATH, stagingDir, L"Updater.exe")) ||
				GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES) {
				// Fall back to the installed copy only if the package lacks one.
				if (FAILED(PathCchCombine(source, MAX_PATH, installDir, L"Updater.exe")) ||
					GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES) {
					AppendUpdateLog("Updater.exe missing in staged package and install dir");
					return false;
				}
				AppendUpdateLog("staged Updater.exe missing; using installed copy");
			}
			wchar_t tempRoot[MAX_PATH] = { 0 };
			if (!GetTempPathW(MAX_PATH, tempRoot)) {
				AppendUpdateLog("GetTempPath failed");
				return false;
			}
			wchar_t leaf[64] = { 0 };
			swprintf_s(leaf, L"sf4e-updater-%lu-%llu", GetCurrentProcessId(),
				static_cast<unsigned long long>(GetTickCount64()));
			wchar_t dir[MAX_PATH] = { 0 };
			if (FAILED(PathCchCombine(dir, MAX_PATH, tempRoot, leaf)) || !CreateDirectoryW(dir, NULL)) {
				AppendUpdateLog("could not create updater temp directory");
				return false;
			}
			if (FAILED(PathCchCombine(outPath, outLen, dir, L"Updater.exe")) ||
				!CopyFileW(source, outPath, TRUE)) {
				AppendUpdateLog("could not copy Updater.exe outside the install");
				return false;
			}
			return true;
		}

		static bool SpawnUpdater(const wchar_t* installDir, const wchar_t* stagingDir, DWORD waitPid) {
			wchar_t updaterPath[MAX_PATH] = { 0 };
			if (!StageUpdaterOutsideInstall(installDir, stagingDir, updaterPath, MAX_PATH)) {
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

		bool WidePathToUtf8(const wchar_t* wide, char* out, int outLen) {
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

		bool EnsureParentDirectoryExistsW(const wchar_t* filePath, std::string& outError) {
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

		void AppendUpdateLog(const char* message) {
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

	} // namespace detail
	using namespace detail;

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
			result.error = loc::T("update.github_unreachable");
			return result;
		}

        return ParseGithubReleaseResponse(body, installed);
    }

    UpdateCheckResult ParseGithubReleaseResponse(const std::string& body, const char* installed) {
        UpdateCheckResult result;
        if (!installed) installed = "";
        result.installedVersion = installed;
		try {
			nlohmann::json release = nlohmann::json::parse(body);
			result.latestVersion = release.value("tag_name", "");
			result.releaseNotes = release.value("body", "");
			result.releaseUrl = release.value("html_url", "");

			if (result.releaseNotes.size() > 2000) {
				result.releaseNotes = result.releaseNotes.substr(0, 2000) + "...";
			}

			if (result.latestVersion.empty()) {
				result.error = loc::T("update.no_version_tag");
				return result;
			}

			if (release.contains("assets") && release["assets"].is_array()) {
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
					if (name.size() < 4 || name.compare(name.size()-4, 4, ".zip") != 0) {
						continue;
					}
					if (name.compare(0, strlen(kReleaseZipPrefix), kReleaseZipPrefix) == 0) {
						result.zipDownloadUrl = asset.value("browser_download_url", "");
						result.zipApiUrl = asset.value("url", "");
						result.expectedSha256 = parseSha256Digest(asset.value("digest", ""));
						break;
					}

				}

			}

			if (result.zipDownloadUrl.empty()) {
				result.error = loc::T("update.no_zip_asset");
				return result;
			}

			result.updateAvailable = CompareVersions(result.latestVersion.c_str(), installed) > 0;
			result.ok = true;
		}
		catch (...) {
			result.error = loc::T("update.parse_failed");
		}
		return result;
	}

	ApplyUpdateResult DownloadAndApplyUpdate(
		const char* zipDownloadUrl,
		const char* zipApiUrl,
		const char* latestVersionTag,
		const char* expectedSha256,
        const std::function<bool(std::uint64_t, std::uint64_t)>& progress
	) {
		ApplyUpdateResult result;
		if ((!zipDownloadUrl || !zipDownloadUrl[0]) && (!zipApiUrl || !zipApiUrl[0])) {
			result.error = loc::T("update.missing_url");
			return result;
		}
		if (!latestVersionTag || !latestVersionTag[0]) {
			result.error = loc::T("update.missing_version");
			return result;
		}
		if (IsGameProcessRunning()) {
			result.error = loc::T("update.close_sf4");
			return result;
		}

		wchar_t installDir[MAX_PATH] = { 0 };
		if (!GetLauncherInstallDir(installDir, MAX_PATH)) {
			result.error = loc::T("update.install_dir_failed");
			return result;
		}

		char safeTag[64] = { 0 };
		SanitizeTagForPath(latestVersionTag, safeTag, sizeof(safeTag));

		wchar_t tempRoot[MAX_PATH] = { 0 };
		std::string tempPathError;
		if (!BuildUpdateTempRoot(safeTag, tempRoot, MAX_PATH, tempPathError)) {
			result.error = loc::Tf("update.temp_path_failed", tempPathError);
			return result;
		}

		char tempRootUtf8[MAX_PATH * 2] = { 0 };
		WidePathToUtf8(tempRoot, tempRootUtf8, sizeof(tempRootUtf8));
		AppendUpdateLog(("update temp dir: " + std::string(tempRootUtf8)).c_str());

		if (!EnsureDirectoryExistsW(tempRoot, tempPathError)) {
			AppendUpdateLog(("update temp mkdir failed: " + tempPathError).c_str());
			result.error = loc::Tf("update.temp_folder_failed", tempPathError);
			return result;
		}

		wchar_t tempBase[MAX_PATH] = { 0 };
		if (GetTempPathW(MAX_PATH, tempBase) == 0) {
			result.error = loc::T("update.temp_access_failed");
			return result;
		}

		wchar_t zipPath[MAX_PATH] = { 0 };
		wchar_t zipName[128] = { 0 };
		swprintf_s(zipName, L"sf4-netplay-update-package-%hs.zip", safeTag);
		if (FAILED(PathCchCombine(zipPath, MAX_PATH, tempBase, zipName))) {
			result.error = loc::T("update.zip_path_failed");
			return result;
		}

		char zipPathUtf8[MAX_PATH * 2] = { 0 };
		WidePathToUtf8(zipPath, zipPathUtf8, sizeof(zipPathUtf8));
		AppendUpdateLog(("update zip path: " + std::string(zipPathUtf8)).c_str());

		wchar_t extractDir[MAX_PATH] = { 0 };
		PathCchCombine(extractDir, MAX_PATH, tempRoot, L"extract");
		if (!EnsureDirectoryExistsW(extractDir, tempPathError)) {
			AppendUpdateLog(("update extract mkdir failed: " + tempPathError).c_str());
			result.error = loc::Tf("update.extract_folder_failed", tempPathError);
			return result;
		}

		std::string downloadError;
		AppendUpdateLog("DownloadAndApplyUpdate start");
		if (!DownloadReleaseZip(zipApiUrl, zipDownloadUrl, zipPath, downloadError, progress)) {
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
			result.error = loc::Tf("update.download_failed", downloadError, releasePage);
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
				result.error = loc::T("update.verify_failed");
				return result;
			}
			if (!HexEqualsIgnoreCase(actualHash, expectedHash)) {
				AppendUpdateLog(("hash mismatch expected=" + expectedHash + " actual=" + actualHash).c_str());
				result.error = loc::T("update.integrity_failed");
				return result;
			}
			AppendUpdateLog("hash verification ok");
		} else {
			result.error = loc::T("update.no_digest"); return result;
		}

		if (!ExpandZipArchive(zipPath, extractDir)) {
			AppendUpdateLog("extract failed");
			result.error = loc::T("update.extract_failed");
			return result;
		}
		AppendUpdateLog("extract ok");

		if (!ValidateExtractedTree(extractDir)) {
			AppendUpdateLog("extract path validation failed");
			result.error = loc::T("update.invalid_paths");
			return result;
		}
		AppendUpdateLog("extract path validation ok");

		wchar_t stagingDir[MAX_PATH] = { 0 };
		if (!FindPackageRoot(extractDir, stagingDir, MAX_PATH)) {
			AppendUpdateLog("package root not found after extract");
			result.error = loc::T("update.missing_binaries");
			return result;
		}

		char stagingUtf8[MAX_PATH * 2] = { 0 };
		if (!WideToUtf8(stagingDir, stagingUtf8, sizeof(stagingUtf8))) {
			result.error = loc::T("update.staging_path_failed");
			return result;
		}
		AppendUpdateLog(("staging dir: " + std::string(stagingUtf8)).c_str());
		if (!ValidateStagedPackage(stagingDir)) {
			AppendUpdateLog("package validation failed");
			result.error = loc::T("update.validation_failed");
			return result;
		}
		AppendUpdateLog("package validation ok");

        if (progress && !progress(0,0)) { result.error = loc::T("update.cancelled"); return result; }
        if (IsGameProcessRunning()) { result.error = loc::T("update.close_game"); return result; }
        if (!SpawnUpdater(installDir, stagingDir, GetCurrentProcessId())) {
			result.error = loc::T("update.updater_start_failed");
			return result;
		}

		AppendUpdateLog("DownloadAndApplyUpdate complete");

		result.ok = true;
		return result;
	}

} // namespace launcher
} // namespace sf4e
