#pragma once

#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "github_release_client.hxx"

namespace sf4e {
namespace launcher {

	// The update a player accepted in the game: the channel, the release tag and
	// the package's SHA-256. It travels to the Launcher as
	// `--install-offer <channel> <tag> <sha256>`, and the Updates window installs
	// it straight away only while a fresh check still finds exactly this release.
	struct InstallOffer {
		UpdateChannel channel = UpdateChannel::Stable;
		std::string tag, sha256;
		bool operator==(const InstallOffer& other) const {
			return channel == other.channel && tag == other.tag && sha256 == other.sha256;
		}
		bool operator!=(const InstallOffer& other) const { return !(*this == other); }
	};

	inline std::string LowerHex(std::string text) {
		for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return text;
	}
	inline bool IsSha256Hex(const std::string& text) {
		if (text.size() != 64) return false;
		for (const char c : text) if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
		return true;
	}
	// A tag the command line can carry as one plain word: a version label
	// (ParseVersion) made of letters, digits, dots, dashes and underscores.
	inline bool IsOfferTag(const std::string& tag) {
		if (tag.empty() || tag.size() > 64 || !ParseVersion(tag.c_str())) return false;
		for (const char c : tag)
			if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_') return false;
		return true;
	}

	// The three words after --install-offer, or nothing when any of them is not
	// what Ember wrote: an unknown channel, a tag that is not a version, a
	// digest that is not 64 hex digits.
	inline std::optional<InstallOffer> ParseInstallOffer(const std::vector<std::string>& words) {
		if (words.size() != 3) return std::nullopt;
		const auto channel = updates::ParseSavedUpdateChannel(words[0]);
		if (!channel || !IsOfferTag(words[1]) || !IsSha256Hex(words[2])) return std::nullopt;
		return InstallOffer{*channel, words[1], LowerHex(words[2])};
	}

	// The arguments that hand `offer` to the Launcher, led by a space.
	inline std::wstring InstallOfferArguments(const InstallOffer& offer) {
		const std::string text = std::string(" --install-offer ") + updates::GetUpdateChannelInfo(offer.channel).stored + " " + offer.tag + " " + offer.sha256;
		return std::wstring(text.begin(), text.end());  // ASCII, by the checks above
	}

	// What a check found, as an offer a player can accept: a verifiable package
	// of the release the check offers, newer or a step back. A release without a
	// digest and a tag that cannot travel are never offered this way.
	inline std::optional<InstallOffer> OfferFrom(UpdateChannel channel, const UpdateCheckResult& result) {
		if (!result.ok || !result.updateAvailable || !IsOfferTag(result.latestVersion) ||
			!IsSha256Hex(result.expectedSha256)) return std::nullopt;
		return InstallOffer{channel, result.latestVersion, LowerHex(result.expectedSha256)};
	}

	// Whether a fresh check finds exactly the accepted release: same channel,
	// same tag, same package digest.
	inline bool OfferStillCurrent(const InstallOffer& offer, UpdateChannel channel, const UpdateCheckResult& result) {
		const auto found = OfferFrom(channel, result);
		return found && *found == offer;
	}

	// The Updates window's handling of an offer from the game. It decides once,
	// when the window's own check has finished: Install while that check finds
	// exactly the accepted release, otherwise the window shows its normal explicit
	// offer (Changed: another release is now current; Gone: there is no update to
	// offer). Nothing is installed on the offer's word alone.
	class InstallOfferGate {
	public:
		enum class Step { Wait, Install, Changed, Gone };
		explicit InstallOfferGate(std::optional<InstallOffer> offer = std::nullopt) : offer_(std::move(offer)) {}
		// `checked`: the window's check has finished with `result`.
		Step Observe(UpdateChannel channel, const UpdateCheckResult& result, bool checked) {
			if (!offer_ || settled_ || !checked) return Step::Wait;
			settled_ = true;
			if (OfferStillCurrent(*offer_, channel, result)) return Step::Install;
			return result.ok && result.updateAvailable ? Step::Changed : Step::Gone;
		}
	private:
		std::optional<InstallOffer> offer_;
		bool settled_ = false;
	};

} // namespace launcher
} // namespace sf4e
