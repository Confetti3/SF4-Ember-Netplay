#pragma once

#include <string>
#include <functional>
#include <cstdint>

namespace sf4e {
namespace launcher {

	struct UpdateCheckResult {
		bool ok = false;
		std::string error;
		std::string installedVersion;
		std::string latestVersion;
		bool updateAvailable = false;
		std::string releaseNotes;
		std::string releaseUrl;
		std::string zipDownloadUrl;
		std::string zipApiUrl;
		// Lowercase hex SHA-256 of the release zip, from the GitHub asset's
		// "digest" field ("sha256:<hex>"). Empty if the release predates GitHub
		// asset digests; callers should treat an empty value as "unverifiable".
		std::string expectedSha256;
	};

	struct ApplyUpdateResult {
		bool ok = false;
		std::string error;
	};

	bool GetLauncherInstallDir(wchar_t* outDir, int outDirChars);
	// When an update was interrupted (the install still holds its transaction
	// journal), starts the Updater to restore it once process waitPid exits;
	// the Updater then starts the Launcher again. True means the caller must
	// exit now. False means nothing is pending or recovery could not start
	// (ledger H-013).
	bool StartPendingUpdateRecovery(std::uint32_t waitPid);
	bool ReadInstalledVersion(char* outVersion, int outVersionLen);
	bool IsGameProcessRunning();

    constexpr const char* kDefaultGithubRepo = "Confetti3/SF4-Ember-Netplay";
    constexpr const char* kReleaseZipPrefix = "sf4-ember-netplay-";
    // Pure release parsing; HTTP and installation remain separate.
    UpdateCheckResult ParseGithubReleaseResponse(const std::string& body, const char* installed);
	UpdateCheckResult CheckForUpdate();
	ApplyUpdateResult DownloadAndApplyUpdate(
		const char* zipDownloadUrl,
		const char* zipApiUrl,
		const char* latestVersionTag,
		const char* expectedSha256,
        const std::function<bool(std::uint64_t, std::uint64_t)>& progress = {}
	);

} // namespace launcher
} // namespace sf4e
