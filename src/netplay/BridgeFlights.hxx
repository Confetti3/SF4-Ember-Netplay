#pragma once
// The bridge requests the runtime has sent the helper and not yet had an
// answer to, and what each does with its answer. One place keeps the rules
// every such request shares, so a feature that asks the bridge for something
// writes only what it does with the answer:
//
// - the answer finds its request by the helper's request id (one map lookup);
// - a request the helper never answers is ended after GiveUpMs, as
//   "unavailable";
// - a request that could not be handed to the helper is ended at once, as
//   "helper_unavailable";
// - a refusal that names no reason reads as "unavailable".
//
// The helper bounds its own bridge requests; GiveUpMs only ends a wait for one
// it lost. Pure, so the rules are tested without a helper (bridge_flights_test.cxx).
#include "../session/TournamentAnswers.hxx"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace netplay {

class BridgeFlights {
public:
	using Handler = std::function<void(const session::TournamentAnswer&)>;

	static constexpr std::uint64_t GiveUpMs = 60000;
	static constexpr const char* Unavailable = "unavailable";
	static constexpr const char* HelperUnavailable = "helper_unavailable";

	// A request handed to the helper as `id` at `nowMs`, whose answer `handler`
	// takes. `id` 0 means it could not be handed over: `handler` is told so now,
	// as a refusal, before this returns. Returns `id`.
	std::uint64_t Begin(std::uint64_t id, Handler handler, std::uint64_t nowMs) {
		if (!id) {
			handler(Refused(HelperUnavailable));
			return 0;
		}
		flights_[id] = Flight{std::move(handler), nowMs};
		return id;
	}

	// The answer: it goes to the request's handler, once. False when no
	// request awaits it (it was never sent, was forgotten, or already ended).
	bool Take(const session::TournamentAnswer& answer) {
		const auto found = answer.requestId ? flights_.find(answer.requestId) : flights_.end();
		if (found == flights_.end()) return false;
		auto handler = std::move(found->second.handler);
		flights_.erase(found);
		handler(answer);
		return true;
	}

	// Ends every request that has waited GiveUpMs. A handler may send another.
	void Expire(std::uint64_t nowMs) {
		std::vector<Handler> lost;
		for (auto flight = flights_.begin(); flight != flights_.end();) {
			if (nowMs - flight->second.sentMs >= GiveUpMs) {
				lost.push_back(std::move(flight->second.handler));
				flight = flights_.erase(flight);
			} else {
				++flight;
			}
		}
		for (auto& handler : lost) handler(Refused(Unavailable));
	}

	bool Awaiting(std::uint64_t id) const { return id && flights_.count(id) != 0; }
	std::size_t Count() const { return flights_.size(); }

	// The request no longer matters: its handler is never called.
	void Forget(std::uint64_t id) { flights_.erase(id); }

	// The reason a refused answer gives, "unavailable" when it names none.
	static std::string Reason(const session::TournamentAnswer& answer) { return answer.reason.empty() ? std::string(Unavailable) : answer.reason; }

private:
	struct Flight {
		Handler handler;
		std::uint64_t sentMs = 0;
	};

	static session::TournamentAnswer Refused(const char* reason) {
		session::TournamentAnswer answer;
		answer.ok = false;
		answer.reason = reason;
		return answer;
	}

	std::map<std::uint64_t, Flight> flights_;
};

} }
