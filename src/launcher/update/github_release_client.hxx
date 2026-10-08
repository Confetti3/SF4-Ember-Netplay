#pragma once

#include <string>
#include <functional>
#include <optional>
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
		// The offer goes back a version: the Stable channel's best release is
		// older than the installed pre-release. Installing it is a downgrade
		// the player chose; an install is never given one otherwise.
		bool goesBack = false;
	};

	// A release tag or installed version label: "v1.1.0", "1.1.0-rc2",
	// "1.1.0-links-sets-test1". Anything with another shape is not a version
	// and is never offered or compared. A pre-release sorts before the finished
	// release of its number, then by its word, its number and the rest.
	struct Version {
		int major = 0, minor = 0, patch = 0;
		bool prerelease = false;
		std::string word, rest;
		int number = 0;
	};
	std::optional<Version> ParseVersion(const char* text);
	int CompareVersions(const Version& a, const Version& b);

	// Stable offers finished releases; Pre-release also betas and release
	// candidates. Unchosen, the channel follows the installed version.
	enum class UpdateChannel { Stable, Prerelease };
	const char* UpdateChannelName(UpdateChannel channel);
	UpdateChannel ResolveUpdateChannel(const std::string& saved, const char* installed);
	// Whether installing `tag` over `installed` is the kind of offer the check
	// made: an update, or with goBack a step back between two valid versions.
	bool TransitionOffered(const char* tag, const char* installed, bool goBack);

	struct ApplyUpdateResult {
		bool ok = false;
		std::string error;
	};

	bool GetLauncherInstallDir(wchar_t* outDir, int outDirChars);
	// When an update was interrupted (the install still holds its transaction
	// journal), starts the Updater to restore it once process waitPid exits;
	// the Updater then starts the Launcher again (ledger H-013). Started means
	// the caller must exit now; Failed means an install is half replaced and
	// the game must not start; NotNormalUser is Failed because Ember was started
	// as administrator, and starting it normally lets the recovery run.
	enum class PendingRecovery { None, Started, Failed, NotNormalUser };
	PendingRecovery StartPendingUpdateRecovery(std::uint32_t waitPid);
	bool ReadInstalledVersion(char* outVersion, int outVersionLen);
	bool IsGameProcessRunning();

    constexpr const char* kDefaultGithubRepo = "Confetti3/SF4-Ember-Netplay";
    constexpr const char* kReleaseZipPrefix = "sf4-ember-netplay-";
    // Pure release parsing; HTTP and installation remain separate. The
    // channel's highest listed release with a package, and whether it is
    // newer than the installed version.
    UpdateCheckResult ParseGithubReleases(const std::string& body, const char* installed, UpdateChannel channel);
	UpdateCheckResult CheckForUpdate(UpdateChannel channel);
	// The steps DownloadAndApplyUpdate goes through, for the status line, each
	// with how far it is: bytes while downloading and verifying, files after.
	// A total of 0 is not known. Returning false cancels the update.
	enum class UpdateStage { Downloading, Verifying, Extracting, Preparing };
	using UpdateProgress = std::function<bool(UpdateStage, std::uint64_t done, std::uint64_t total)>;
	ApplyUpdateResult DownloadAndApplyUpdate(
		const char* zipDownloadUrl,
		const char* zipApiUrl,
		const char* latestVersionTag,
		const char* expectedSha256,
		bool goBack,
        const UpdateProgress& progress = {},
        // Shown by Updater.exe while it installs; it has no catalogs of its own.
        const char* installingText = ""
	);

} // namespace launcher
} // namespace sf4e
