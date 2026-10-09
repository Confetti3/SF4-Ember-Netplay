#pragma once

#include <string>
#include <functional>
#include <optional>
#include <cstdint>
#include "../../common/UpdateChannel.hxx"
#include "PackageInstaller.hxx"

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
		// The offer goes back a version: the channel's best release is older
		// than an installed version of a kind the channel does not accept.
		bool goesBack = false;
	};

	// A release tag or installed version label: "v1.1.0", "1.1.0-rc2",
	// "1.1.0-links-sets-test1". Anything with another shape is not a version
	// and is never offered or compared. A pre-release sorts before the finished
	// release of its number, then by its word, its number and the rest.
	// Dot-separated numeric Nightly revisions in the rest sort numerically.
	struct Version {
		int major = 0, minor = 0, patch = 0;
		bool prerelease = false;
		std::string word, rest;
		int number = 0;
	};
	std::optional<Version> ParseVersion(const char* text);
	int CompareVersions(const Version& a, const Version& b);

	using updates::ReleaseKind;
	using updates::UpdateChannel;
	using updates::UpdateChannelInfo;
	using updates::kDefaultGithubRepo;
	using updates::kUpdateChannels;
	using updates::GetUpdateChannelInfo;
	using updates::ParseSavedUpdateChannel;
	ReleaseKind ClassifyReleaseKind(const Version& version);

	const char* UpdateChannelName(UpdateChannel channel);
	const char* UpdateChannelRepo(UpdateChannel channel);
	UpdateChannel ResolveUpdateChannel(std::optional<UpdateChannel> saved, const char* installed);
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

    constexpr const char* kReleaseZipPrefix = "sf4-ember-netplay-";
    // Pure release parsing; HTTP and installation remain separate. The
    // channel's highest listed release with a package, and whether it is
    // newer than the installed version.
    UpdateCheckResult ParseGithubReleases(const std::string& body, const char* installed, UpdateChannel channel);
	UpdateCheckResult CheckForUpdate(UpdateChannel channel);
	// Goes through Downloading, Verifying, Extracting and Preparing, reporting
	// each to `progress` for the status line. Each step starts with a report of
	// (0, 0). Setting `cancel` stops the update with "update.cancelled", up to
	// the moment the Updater would start.
	ApplyUpdateResult DownloadAndApplyUpdate(
		const UpdateCheckResult& offer,
        const std::atomic<bool>& cancel,
        const Progress& progress = {},
        // Shown by Updater.exe while it installs; it has no catalogs of its own.
        const char* installingText = ""
	);

} // namespace launcher
} // namespace sf4e
