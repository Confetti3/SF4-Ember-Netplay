#pragma once
// When the Archive tab asks for a page of the listing. The tab wants one
// filter on one service; the runtime publishes what it holds (Status). This
// decides from the two what to ask for now, so a page is asked for once, a
// changed filter asks for the first page again at once, and the next page
// follows when the player reaches the end of the list. No ImGui
// (archive_requests_test.cxx).
#include "../netplay/Archive.hxx"
#include <string>

namespace sf4e { namespace ui { namespace archivepaging {

enum class Ask {
	None,
	// The first page of the wanted filter.
	First,
	// The page after the last one heard, or that page again after it failed.
	Next,
	NextAgain,
};

// What the tab has asked for that the runtime's status does not show yet: the
// command takes a tick to reach it.
struct Memory {
	std::string bridge;
	netplay::archive::Filter filter;
	bool first = false;
	std::string next;
};

// Whether the runtime's status is the listing of `wanted` on `bridge`.
inline bool Heard(const netplay::archive::Status& status, const std::string& bridge, const netplay::archive::Filter& wanted) {
	return status.state != netplay::archive::Status::State::Idle && status.bridge == bridge && status.filter == wanted;
}

// What to ask for now. `nearEnd`: the player is at the end of the list shown.
// `retry`: the player asked to try again (a failed page) or to refresh.
inline Ask NextAsk(const netplay::archive::Status& status, const std::string& bridge, const netplay::archive::Filter& wanted, bool nearEnd, bool retry,
	Memory& memory) {
	using State = netplay::archive::Status::State;
	if (bridge.empty()) return Ask::None;
	if (!Heard(status, bridge, wanted)) {
		// Asked already, and the answer to that is what the status will show.
		if (!retry && memory.first && memory.bridge == bridge && memory.filter == wanted) return Ask::None;
		memory.bridge = bridge;
		memory.filter = wanted;
		memory.first = true;
		memory.next.clear();
		return Ask::First;
	}
	memory.first = false;
	if (retry) {
		if (status.state == State::Listed && status.moreFailed) {
			memory.next.clear();
			return Ask::NextAgain;
		}
		// Refresh, or try again after a failure: the first page again.
		if (status.state != State::Loading) {
			memory.first = true;
			memory.next.clear();
			return Ask::First;
		}
		return Ask::None;
	}
	if (status.state != State::Listed || !status.listing) return Ask::None;
	if (!nearEnd || status.loadingMore || status.moreFailed || status.listing->next.empty()) return Ask::None;
	// Asked already, and the page after it has not come.
	if (memory.next == status.listing->next) return Ask::None;
	memory.next = status.listing->next;
	return Ask::Next;
}

// Whether the end of a list of `rows` entries is near the focused one at `at` (-1 when focus is not on an entry):
// the last few, or any when the whole list is short.
inline bool NearEnd(int at, std::size_t rows, std::size_t ahead = 6) {
	if (rows <= ahead) return true;
	return at >= 0 && static_cast<std::size_t>(at) + ahead >= rows;
}

} } }
