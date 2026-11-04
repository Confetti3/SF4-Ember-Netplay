#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace sf4e { namespace updates {

	enum class ReleaseKind { Stable, Beta, Nightly };
	enum class UpdateChannel { Stable, Beta, Nightly };
	// Which of the product's repositories (ember::ProductIdentity) a channel reads.
	enum class ReleaseRepo { Stable, Nightly };
	struct UpdateChannelInfo {
		UpdateChannel channel;
		const char* stored;
		ReleaseRepo repo;
		const char* labelKey;
		const char* detailKey;
		ReleaseKind kind;
		std::array<bool, 3> acceptedKinds;
		bool skipGithubPrerelease;
		UpdateChannel next;
		bool Accepts(ReleaseKind releaseKind) const { return acceptedKinds[static_cast<std::size_t>(releaseKind)]; }
	};
	inline constexpr UpdateChannelInfo kUpdateChannels[] = {
		{UpdateChannel::Stable, "stable", ReleaseRepo::Stable, "updates.channel.stable", "updates.channel_detail",
			ReleaseKind::Stable, {true, false, false}, true, UpdateChannel::Beta},
		{UpdateChannel::Beta, "prerelease", ReleaseRepo::Stable, "updates.channel.prerelease", "updates.channel_detail",
			ReleaseKind::Beta, {true, true, false}, false, UpdateChannel::Nightly},
		{UpdateChannel::Nightly, "nightly", ReleaseRepo::Nightly, "updates.channel.nightly", "updates.channel.nightly_detail",
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
