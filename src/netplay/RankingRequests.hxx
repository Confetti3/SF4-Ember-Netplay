#pragma once
// The runtime's half of rankings: which helper requests are in flight, and what
// each answer does to the Status the interface reads (Rankings.hxx). The
// runtime sends the requests and decodes the answers; this decides, so the
// rules are tested without a helper (ranking_requests_test.cxx).
//
// - The runtime sends each request through BridgeFlights, which routes the
//   answer, gives up on a lost one and ends one the helper could not take; the
//   answer comes here under the flight it began with.
// - A standing is one resource read by GET and written by a choice. A choice
//   retires a read already on its way, which may have read the standing before
//   the choice lands, and no read goes while a choice is in flight: the
//   choice's own answer is the standing after it.
// - Answers belong to one service and one Ember ID. A change of either forgets
//   every answer and retires every request; a choice in flight is answered as
//   unavailable.
// - A value's revision moves only when a value is installed, from a read or a
//   choice, and the value keeps the time it arrived; a request's finished
//   count moves whenever it ends. A failed refresh keeps the value, and how
//   old it is.
#include "Rankings.hxx"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace netplay { namespace rankings {

class Requests {
public:
	enum class Kind { None, Standing, Board, Choice };

	// The status as of `nowMs`, the runtime's clock.
	Status Published(std::uint64_t nowMs) const {
		Status status = status_;
		status.nowMs = nowMs;
		return status;
	}
	const std::string& Bridge() const { return status_.bridge; }
	const std::string& EmberId() const { return status_.emberId; }

	// The service and Ember ID the next request is for.
	void Scope(const std::string& bridge, const std::string& emberId) {
		if (bridge == status_.bridge && emberId == status_.emberId) return;
		if (choice_) EndChoice("unavailable", std::nullopt, 0);
		Status fresh;
		fresh.bridge = bridge;
		fresh.emberId = emberId;
		fresh.standing.finished = status_.standing.finished;
		fresh.standing.revision = status_.standing.revision;
		fresh.board.finished = status_.board.finished;
		fresh.board.revision = status_.board.revision;
		fresh.chose = status_.chose;
		fresh.choiceFailure = status_.choiceFailure;
		status_ = std::move(fresh);
		standing_ = board_ = choice_ = 0;
	}

	bool MayReadStanding() const { return !standing_ && !choice_; }
	bool MayReadBoard() const { return !board_; }

	// A request is going to the helper: returns the flight its answer comes back under (the Finish of its kind).
	std::uint64_t BeginStanding() { return Start(status_.standing, standing_); }
	std::uint64_t BeginBoard() { return Start(status_.board, board_); }
	// The interface's choice identified as `request`. A newer one supersedes one
	// in flight, whose answer then names no request.
	std::uint64_t BeginChoice(std::uint64_t request) {
		status_.choosing = request;
		standing_ = 0;
		status_.standing.loading = false;
		return choice_ = ++flights_;
	}

	// Which request in flight `flight` is; None for one retired.
	Kind Awaiting(std::uint64_t flight) const {
		if (!flight) return Kind::None;
		return flight == standing_ ? Kind::Standing : flight == board_ ? Kind::Board : flight == choice_ ? Kind::Choice : Kind::None;
	}
	// The answer, at `nowMs`, to `flight`: its value, or why there is none (the
	// helper's code, or "bridge_invalid_response"). The answer to a retired
	// flight changes nothing.
	void FinishStanding(std::uint64_t flight, const std::string& failure, std::optional<Standing> standing, std::uint64_t nowMs) {
		if (Awaiting(flight) != Kind::Standing) return;
		if (standing) Install(std::move(*standing), nowMs);
		End(status_.standing, standing_, failure);
	}
	void FinishBoard(std::uint64_t flight, const std::string& failure, std::optional<std::vector<BoardEntry>> board, std::uint64_t nowMs) {
		if (Awaiting(flight) != Kind::Board) return;
		if (board) {
			status_.board.value = std::make_shared<const std::vector<BoardEntry>>(std::move(*board));
			status_.board.loaded = true;
			status_.board.installedMs = nowMs;
			++status_.board.revision;
		}
		End(status_.board, board_, failure);
	}
	void FinishChoice(std::uint64_t flight, const std::string& failure, std::optional<Standing> standing, std::uint64_t nowMs) {
		if (Awaiting(flight) == Kind::Choice) EndChoice(failure, std::move(standing), nowMs);
	}

private:
	template <class Value>
	std::uint64_t Start(Answered<Value>& answered, std::uint64_t& flight) {
		flight = ++flights_;
		answered.loading = true;
		answered.error.clear();
		return flight;
	}
	template <class Value>
	static void End(Answered<Value>& answered, std::uint64_t& flight, const std::string& failure) {
		flight = 0;
		answered.loading = false;
		answered.error = failure;
		++answered.finished;
	}
	void EndChoice(const std::string& failure, std::optional<Standing> standing, std::uint64_t nowMs) {
		if (standing) Install(std::move(*standing), nowMs);
		status_.chose = status_.choosing;
		status_.choosing = 0;
		status_.choiceFailure = failure;
		choice_ = 0;
	}
	void Install(Standing standing, std::uint64_t nowMs) {
		status_.standing.value = std::move(standing);
		status_.standing.loaded = true;
		status_.standing.installedMs = nowMs;
		++status_.standing.revision;
	}

	Status status_;
	// The requests on their way (0: none), and how many have begun.
	std::uint64_t standing_ = 0, board_ = 0, choice_ = 0, flights_ = 0;
};

} } }
