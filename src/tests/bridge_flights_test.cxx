// The bridge requests the runtime waits on (netplay/BridgeFlights.hxx): an
// answer finds its request by id and is handled once; a request nobody
// answers is ended after a minute, and one the helper could not take at once,
// both with the reason every feature maps the same way; a request forgotten
// never reaches its handler.
#include "../netplay/BridgeFlights.hxx"

#include <cstdio>
#include <string>
#include <vector>

#include "test_support.hxx"

using sf4e::netplay::BridgeFlights;
using sf4e::session::TournamentAnswer;

namespace {
TournamentAnswer Answer(std::uint64_t id, bool ok, const std::string& reason = {}) {
	TournamentAnswer answer;
	answer.requestId = id;
	answer.ok = ok;
	answer.reason = reason;
	return answer;
}

// What each handler saw, as "name:ok" or "name:reason".
struct Seen {
	std::vector<std::string> log;
	BridgeFlights::Handler Handler(const std::string& name) {
		return [this, name](const TournamentAnswer& answer) { log.push_back(name + ":" + (answer.ok ? "ok" : BridgeFlights::Reason(answer))); };
	}
};

void RoutesById() {
	BridgeFlights flights;
	Seen seen;
	EXPECT(flights.Begin(7, seen.Handler("a"), 100) == 7 && flights.Begin(8, seen.Handler("b"), 100) == 8);
	EXPECT(flights.Awaiting(7) && flights.Awaiting(8) && !flights.Awaiting(9) && !flights.Awaiting(0) && flights.Count() == 2);
	EXPECT(flights.Take(Answer(8, true)) && seen.log == std::vector<std::string>{"b:ok"});
	// Once: the same answer again, and one nobody sent, are nobody's.
	EXPECT(!flights.Take(Answer(8, true)) && !flights.Take(Answer(9, true)) && !flights.Take(Answer(0, true)));
	EXPECT(flights.Take(Answer(7, false, "room_full")) && seen.log.back() == "a:room_full" && flights.Count() == 0);
}

// A refusal that names no reason is "unavailable", written once.
void EmptyReason() {
	BridgeFlights flights;
	Seen seen;
	flights.Begin(1, seen.Handler("a"), 0);
	flights.Take(Answer(1, false));
	EXPECT(seen.log == std::vector<std::string>{"a:unavailable"});
	EXPECT(BridgeFlights::Reason(Answer(1, false, "banned")) == "banned");
}

// A request the helper could not take ends before Begin returns; one it never answers ends after a minute.
void LostAndRefused() {
	BridgeFlights flights;
	Seen seen;
	EXPECT(flights.Begin(0, seen.Handler("none"), 0) == 0 && seen.log == std::vector<std::string>{"none:helper_unavailable"} && flights.Count() == 0);
	flights.Begin(5, seen.Handler("early"), 1000);
	flights.Begin(6, seen.Handler("late"), 30000);
	flights.Expire(1000 + BridgeFlights::GiveUpMs - 1);
	EXPECT(seen.log.size() == 1 && flights.Awaiting(5));
	flights.Expire(1000 + BridgeFlights::GiveUpMs);
	EXPECT(seen.log.size() == 2 && seen.log[1] == "early:unavailable" && !flights.Awaiting(5) && flights.Awaiting(6));
	// Its answer, coming late, is nobody's.
	EXPECT(!flights.Take(Answer(5, true)));
	flights.Expire(30000 + BridgeFlights::GiveUpMs);
	EXPECT(seen.log.size() == 3 && seen.log[2] == "late:unavailable" && flights.Count() == 0);
}

// A handler may send the next request, from inside the answer or the give-up.
void HandlerSends() {
	BridgeFlights flights;
	Seen seen;
	flights.Begin(1, [&](const TournamentAnswer&) { flights.Begin(2, seen.Handler("next"), 10); }, 0);
	EXPECT(flights.Take(Answer(1, true)) && flights.Awaiting(2));
	flights.Begin(3, [&](const TournamentAnswer&) { flights.Begin(4, seen.Handler("again"), 99); }, 0);
	flights.Expire(BridgeFlights::GiveUpMs);
	EXPECT(flights.Awaiting(4) && !flights.Awaiting(3));
}

void Forgetting() {
	BridgeFlights flights;
	Seen seen;
	flights.Begin(1, seen.Handler("a"), 0);
	flights.Forget(1);
	flights.Forget(2);
	EXPECT(!flights.Take(Answer(1, true)) && seen.log.empty());
	flights.Begin(3, seen.Handler("b"), 0);
	flights.Forget(3);
	flights.Expire(BridgeFlights::GiveUpMs * 2);
	EXPECT(seen.log.empty() && flights.Count() == 0);
}
}

int main() {
	RoutesById();
	EmptyReason();
	LostAndRefused();
	HandlerSends();
	Forgetting();
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	return TEST_EXIT_CODE();
}
