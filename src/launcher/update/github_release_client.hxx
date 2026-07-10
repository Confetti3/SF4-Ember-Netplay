#pragma once

#include <string>

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
	bool ReadInstalledVersion(char* outVersion, int outVersionLen);
	bool IsGameProcessRunning();

	UpdateCheckResult CheckForUpdate();
	ApplyUpdateResult DownloadAndApplyUpdate(
		const char* zipDownloadUrl,
		const char* zipApiUrl,
		const char* latestVersionTag,
		const char* expectedSha256
	);

} // namespace launcher
} // namespace sf4e
