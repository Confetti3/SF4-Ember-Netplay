#pragma once
// What the interface sees of the replay channel (docs/design/REPLAY_CHANNEL.md):
// the player's choice to share their room games, a page of the channel, a
// download, and the uploads of the player's own games. The helper answers by
// request id; the runtime decodes each answer completely and Requests decides
// what it does, so the rules are tested without a helper
// (replay_channel_test.cxx).
//
// - Answers belong to one service and one Ember ID, like rankings: a change of
//   either forgets them and retires every request.
// - Uploads run on their own once the player's choice has been read as on:
//   one at a time, the next as soon as one is answered, and after a failure
//   that may pass, again only after a wait that grows. A choice read as off
//   stops them and empties the queue.
#include "Rankings.hxx"
#include "../common/Country.hxx"
#include "../common/SkillBadge.hxx"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace netplay { namespace channel {

constexpr std::size_t PageSize = 50;
using rankings::Answered;

enum class Sharing : std::uint8_t { NoAccount, On, Off };

struct Side {
	std::string name;
	CountryCode country;
	int fighter = -1;
	// None for a player who does not take part in ranking or had no class yet.
	SkillBadge badge;
	bool own = false;
};

struct Entry {
	std::string id;
	std::uint64_t playedAt = 0;
	bool publicRoom = false;
	Side sides[2];
	// The winning side, -1 when the bridge does not know it.
	int winner = -1;
	std::uint32_t size = 0;
};

struct Page {
	std::vector<Entry> entries;
	bool more = false;
};

// Which uploads failed how: the ones that will never go are dropped, the
// rest wait and are tried again.
inline bool UploadFinal(const std::string& failure) {
	return failure == "replay_invalid" || failure == "replay_not_your_game" || failure == "invalid_request" ||
		failure == "invalid_path" || failure == "replay_unreadable";
}

struct Status {
	std::string bridge, emberId;
	Answered<Sharing> sharing;
	// The pages listed so far for `fighter` (-1 for every fighter), newest
	// first: a further page appends to them. Shared, so a frame's copy is cheap.
	Answered<std::shared_ptr<const Page>> page;
	int fighter = -1;
	// The identity (Command::request) of the sharing choice in flight and of
	// the last one answered, with the helper's code when it was refused.
	std::uint64_t choosing = 0, chose = 0;
	std::string choiceFailure;
	// The same for a download, and the file the last one wrote.
	std::uint64_t downloading = 0, downloaded = 0;
	std::string downloadFailure, downloadedFile;
	// Replays waiting to go, as of the last look at the queue, and how many
	// went this session.
	std::size_t queued = 0;
	std::uint64_t uploaded = 0;
	std::uint64_t nowMs = 0;
	template <class Value>
	double AgeSeconds(const Answered<Value>& answered) const {
		return nowMs > answered.installedMs ? static_cast<double>(nowMs - answered.installedMs) / 1000 : 0;
	}
};

class Requests {
public:
	static constexpr std::uint64_t GiveUpMs = 60000;
	static constexpr std::uint64_t FirstWaitMs = 60000, LongestWaitMs = 30 * 60000, UnavailableWaitMs = 60 * 60000;
	enum class Kind { None, Sharing, Choice, Page, Download, Upload };

	Status Published(std::uint64_t nowMs) const {
		Status status = status_;
		status.nowMs = nowMs;
		return status;
	}
	const std::string& Bridge() const { return status_.bridge; }
	const std::string& EmberId() const { return status_.emberId; }

	void Scope(const std::string& bridge, const std::string& emberId) {
		if (bridge == status_.bridge && emberId == status_.emberId) return;
		if (choice_.id) FinishChoice("unavailable", std::nullopt, 0);
		if (download_.id) FinishDownload("unavailable", {}, 0);
		Status fresh;
		fresh.bridge = bridge;
		fresh.emberId = emberId;
		fresh.sharing.finished = status_.sharing.finished;
		fresh.sharing.revision = status_.sharing.revision;
		fresh.page.finished = status_.page.finished;
		fresh.page.revision = status_.page.revision;
		fresh.chose = status_.chose;
		fresh.choiceFailure = status_.choiceFailure;
		fresh.downloaded = status_.downloaded;
		fresh.downloadFailure = status_.downloadFailure;
		fresh.queued = status_.queued;
		fresh.uploaded = status_.uploaded;
		status_ = std::move(fresh);
		sharing_ = page_ = choice_ = download_ = upload_ = {};
		uploadAfterMs_ = 0;
		failures_ = 0;
	}

	bool MayReadSharing() const { return !sharing_.id && !choice_.id; }
	bool MayReadPage() const { return !page_.id; }

	void SentSharing(std::uint64_t id, std::uint64_t nowMs) {
		if (!Start(status_.sharing, sharing_, id, nowMs)) FinishSharing("helper_unavailable", std::nullopt, nowMs);
	}
	// A page for `fighter`, after the entries listed when `more` is set.
	void SentPage(std::uint64_t id, int fighter, bool more, std::uint64_t nowMs) {
		pageFighter_ = fighter;
		pageMore_ = more;
		if (!Start(status_.page, page_, id, nowMs)) FinishPage("helper_unavailable", std::nullopt, nowMs);
	}
	void SentChoice(std::uint64_t request, std::uint64_t id, std::uint64_t nowMs) {
		status_.choosing = request;
		sharing_ = {};
		status_.sharing.loading = false;
		if (!id) { FinishChoice("helper_unavailable", std::nullopt, nowMs); return; }
		choice_ = {id, nowMs};
	}
	void SentDownload(std::uint64_t request, std::uint64_t id, std::uint64_t nowMs) {
		if (download_.id) FinishDownload("unavailable", {}, nowMs);
		status_.downloading = request;
		if (!id) { FinishDownload("helper_unavailable", {}, nowMs); return; }
		download_ = {id, nowMs};
	}

	// Whether an upload may go now: the player shares, nothing is on its way,
	// and no wait after a failure is running.
	bool MayUpload(std::uint64_t nowMs) const { return SharesNow() && !upload_.id && nowMs >= uploadAfterMs_; }
	// The choice read last is on and no other is on its way: a game played now
	// may wait to be shared.
	bool SharesNow() const { return status_.sharing.loaded && status_.sharing.value == Sharing::On && !choice_.id; }
	void SentUpload(std::uint64_t id, std::uint64_t nowMs) {
		if (!id) { Wait(nowMs, false); return; }
		upload_ = {id, nowMs};
	}
	void Queued(std::size_t count) { status_.queued = count; }

	Kind Awaiting(std::uint64_t id) const {
		if (!id) return Kind::None;
		if (id == sharing_.id) return Kind::Sharing;
		if (id == choice_.id) return Kind::Choice;
		if (id == page_.id) return Kind::Page;
		if (id == download_.id) return Kind::Download;
		if (id == upload_.id) return Kind::Upload;
		return Kind::None;
	}

	void FinishSharing(const std::string& failure, std::optional<Sharing> sharing, std::uint64_t nowMs) {
		if (sharing) Install(*sharing, nowMs);
		End(status_.sharing, sharing_, failure);
	}
	void FinishChoice(const std::string& failure, std::optional<Sharing> sharing, std::uint64_t nowMs) {
		if (sharing) Install(*sharing, nowMs);
		status_.chose = status_.choosing;
		status_.choosing = 0;
		status_.choiceFailure = failure;
		choice_ = {};
	}
	void FinishPage(const std::string& failure, std::optional<Page> page, std::uint64_t nowMs) {
		if (page) {
			auto shown = std::make_shared<Page>();
			const bool appending = pageMore_ && status_.page.loaded && status_.page.value && status_.fighter == pageFighter_;
			if (appending) shown->entries = status_.page.value->entries;
			for (auto& entry : page->entries)
				if (std::none_of(shown->entries.begin(), shown->entries.end(), [&](const Entry& e) { return e.id == entry.id; }))
					shown->entries.push_back(std::move(entry));
			shown->more = page->more;
			status_.page.value = std::move(shown);
			status_.page.loaded = true;
			status_.page.installedMs = nowMs;
			status_.fighter = pageFighter_;
			++status_.page.revision;
		}
		End(status_.page, page_, failure);
	}
	void FinishDownload(const std::string& failure, const std::string& file, std::uint64_t) {
		status_.downloaded = status_.downloading;
		status_.downloading = 0;
		status_.downloadFailure = failure;
		status_.downloadedFile = failure.empty() ? file : std::string();
		download_ = {};
	}
	// An upload's answer: gone (sent, or a failure that will never pass, both of
	// which take it out of the queue), or kept for a later try. True when the
	// queue entry is to be removed.
	bool FinishUpload(const std::string& failure, std::uint64_t nowMs) {
		upload_ = {};
		if (failure.empty()) { ++status_.uploaded; failures_ = 0; uploadAfterMs_ = 0; return true; }
		if (UploadFinal(failure)) return true;
		Wait(nowMs, failure == "replay_channel_unavailable" || failure == "replay_upload_limit");
		// No account means not shared at all: the choice is read again.
		if (failure == "no_discord_account") status_.sharing.loaded = false;
		return false;
	}

	void Expire(std::uint64_t nowMs) {
		if (Lost(sharing_, nowMs)) FinishSharing("unavailable", std::nullopt, nowMs);
		if (Lost(page_, nowMs)) FinishPage("unavailable", std::nullopt, nowMs);
		if (Lost(choice_, nowMs)) FinishChoice("unavailable", std::nullopt, nowMs);
		if (Lost(download_, nowMs)) FinishDownload("unavailable", {}, nowMs);
		if (Lost(upload_, nowMs)) FinishUpload("unavailable", nowMs);
	}

private:
	struct InFlight {
		std::uint64_t id = 0, sentMs = 0;
	};
	template <class Value>
	static bool Start(Answered<Value>& answered, InFlight& flight, std::uint64_t id, std::uint64_t nowMs) {
		if (!id) return false;
		flight = {id, nowMs};
		answered.loading = true;
		answered.error.clear();
		return true;
	}
	template <class Value>
	static void End(Answered<Value>& answered, InFlight& flight, const std::string& failure) {
		flight = {};
		answered.loading = false;
		answered.error = failure;
		++answered.finished;
	}
	static bool Lost(const InFlight& flight, std::uint64_t nowMs) { return flight.id && nowMs - flight.sentMs >= GiveUpMs; }
	void Install(Sharing sharing, std::uint64_t nowMs) {
		status_.sharing.value = sharing;
		status_.sharing.loaded = true;
		status_.sharing.installedMs = nowMs;
		++status_.sharing.revision;
	}
	void Wait(std::uint64_t nowMs, bool unavailable) {
		failures_ = (std::min)(failures_ + 1, 16u);
		std::uint64_t wait = FirstWaitMs << (std::min)(failures_ - 1, 5u);
		wait = (std::min)(wait, LongestWaitMs);
		if (unavailable) wait = UnavailableWaitMs;
		uploadAfterMs_ = nowMs + wait;
	}

	Status status_;
	InFlight sharing_, page_, choice_, download_, upload_;
	int pageFighter_ = -1;
	bool pageMore_ = false;
	std::uint64_t uploadAfterMs_ = 0;
	unsigned failures_ = 0;
};

} } }
