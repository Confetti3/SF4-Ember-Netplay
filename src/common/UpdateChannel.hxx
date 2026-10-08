#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace sf4e { namespace updates {

	enum class ReleaseKind { Stable, Beta, Nightly };
	constexpr const char* kDefaultGithubRepo = "Confetti3/SF4-Ember-Netplay";
	enum class UpdateChannel { Stable, Beta, Nightly };
	struct UpdateChannelInfo {
		UpdateChannel channel;
		const char* stored;
		const char* repo;
		const char* labelKey;
		const char* detailKey;
		ReleaseKind kind;
		std::array<bool, 3> acceptedKinds;
		bool skipGithubPrerelease;
		UpdateChannel next;
		bool Accepts(ReleaseKind releaseKind) const { return acceptedKinds[static_cast<std::size_t>(releaseKind)]; }
	};
	inline constexpr UpdateChannelInfo kUpdateChannels[] = {
		{UpdateChannel::Stable, "stable", kDefaultGithubRepo, "updates.channel.stable", "updates.channel_detail",
			ReleaseKind::Stable, {true, false, false}, true, UpdateChannel::Beta},
		{UpdateChannel::Beta, "prerelease", kDefaultGithubRepo, "updates.channel.prerelease", "updates.channel_detail",
			ReleaseKind::Beta, {true, true, false}, false, UpdateChannel::Nightly},
		{UpdateChannel::Nightly, "nightly", "Confetti3/SF4-Ember-Netplay-Nightly", "updates.channel.nightly", "updates.channel.nightly_detail",
			ReleaseKind::Nightly, {false, false, true}, false, UpdateChannel::Stable}
	};
	inline const UpdateChannelInfo& GetUpdateChannelInfo(UpdateChannel channel) {
		for (const auto& info : kUpdateChannels) if (info.channel == channel) return info;
		return kUpdateChannels[0];
	}

	// Saved values are explicit choices. Absence and unknown values are not
	// channels; only the release resolver infers from the installed version.
	inline std::optional<UpdateChannel> ParseSavedUpdateChannel(std::string_view saved) {
		for (const auto& info : kUpdateChannels) if (saved == info.stored) return info.channel;
		return std::nullopt;
	}

} } // namespace sf4e::updates
