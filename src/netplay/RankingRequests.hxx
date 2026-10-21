#pragma once
// The runtime's half of rankings: which helper requests are in flight, and what
// each answer does to the Status the interface reads (Rankings.hxx). The
// runtime sends the requests and decodes the answers; this decides, so the
// rules are tested without a helper (ranking_requests_test.cxx).
//
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
	// The helper bounds its own bridge requests; this only ends a wait for one it lost.
	static constexpr std::uint64_t GiveUpMs = 60000;
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
		if (choice_.id) FinishChoice("unavailable", std::nullopt, 0);
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
		standing_ = board_ = choice_ = {};
	}

	bool MayReadStanding() const { return !standing_.id && !choice_.id; }
	bool MayReadBoard() const { return !board_.id; }

	// A request handed to the helper as `id`; 0 when it could not be, which ends it now.
	void SentStanding(std::uint64_t id, std::uint64_t nowMs) {
		if (!Start(status_.standing, standing_, id, nowMs)) FinishStanding("helper_unavailable", std::nullopt, nowMs);
	}
	void SentBoard(std::uint64_t id, std::uint64_t nowMs) {
		if (!Start(status_.board, board_, id, nowMs)) FinishBoard("helper_unavailable", std::nullopt, nowMs);
	}
	// The interface's choice identified as `request`. A newer one supersedes one
	// in flight, whose answer then names no request.
	void SentChoice(std::uint64_t request, std::uint64_t id, std::uint64_t nowMs) {
		status_.choosing = request;
		standing_ = {};
		status_.standing.loading = false;
		if (!id) { FinishChoice("helper_unavailable", std::nullopt, nowMs); return; }
		choice_ = {id, nowMs};
	}

	// Which request in flight the helper's answer `id` is for.
	Kind Awaiting(std::uint64_t id) const {
		if (!id) return Kind::None;
		return id == standing_.id ? Kind::Standing : id == board_.id ? Kind::Board : id == choice_.id ? Kind::Choice : Kind::None;
	}
	// The answer, at `nowMs`, to the request in flight of that kind: its value,
	// or why there is none (the helper's code, or "bridge_invalid_response").
	void FinishStanding(const std::string& failure, std::optional<Standing> standing, std::uint64_t nowMs) {
		if (standing) Install(std::move(*standing), nowMs);
		End(status_.standing, standing_, failure);
	}
	void FinishBoard(const std::string& failure, std::optional<std::vector<BoardEntry>> board, std::uint64_t nowMs) {
		if (board) {
			status_.board.value = std::make_shared<const std::vector<BoardEntry>>(std::move(*board));
			status_.board.loaded = true;
			status_.board.installedMs = nowMs;
			++status_.board.revision;
		}
		End(status_.board, board_, failure);
	}
	void FinishChoice(const std::string& failure, std::optional<Standing> standing, std::uint64_t nowMs) {
		if (standing) Install(std::move(*standing), nowMs);
		status_.chose = status_.choosing;
		status_.choosing = 0;
		status_.choiceFailure = failure;
		choice_ = {};
	}

	// Ends the requests the helper never answered.
	void Expire(std::uint64_t nowMs) {
		if (Lost(standing_, nowMs)) FinishStanding("unavailable", std::nullopt, nowMs);
		if (Lost(board_, nowMs)) FinishBoard("unavailable", std::nullopt, nowMs);
		if (Lost(choice_, nowMs)) FinishChoice("unavailable", std::nullopt, nowMs);
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
	void Install(Standing standing, std::uint64_t nowMs) {
		status_.standing.value = std::move(standing);
		status_.standing.loaded = true;
		status_.standing.installedMs = nowMs;
		++status_.standing.revision;
	}

	Status status_;
	InFlight standing_, board_, choice_;
};

} } }
