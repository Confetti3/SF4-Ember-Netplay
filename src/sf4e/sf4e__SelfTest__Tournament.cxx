#include "sf4e__SelfTest__Online.hxx"

#include <cstdint>
#include <ctime>
#include <string>

#include "../platform/Utf8.hxx"
#include "../session/TournamentBinding.hxx"
#include "sf4e.hxx"
#include "sf4e__SelfTest__Attack.hxx"

// The tournament test (docs/design/TOURNAMENT_SELFTEST.md): two PCs play one
// match of a local test bridge end to end. Each side makes its Ember ID,
// trusts the bridge and links to its connection with the code the runner
// gives it, then plays the match the runner creates through the runtime's
// own tournament flow. Every step is a command the Ember ID, Linked accounts
// and Matches screens send. The runner (scripts/run-tournament-selftest.ps1)
// and the game talk through files in the folder SF4E_SELFTEST_EXCHANGE names.
namespace sf4e { namespace selftest {
namespace {

using NetplayFacade::RuntimeCommand;
using NetplayFacade::RuntimeSnapshot;
using netplay::IdentityOp;
using netplay::IdentityRequest;
using netplay::tournament::Phase;
using online::Command;
using online::Now;

// The files of the exchange folder. The game writes its Ember ID and build,
// then its claim's fingerprint; the runner writes the link code, then the
// match.
constexpr const wchar_t* kEmberIdFile = L"ember-id";
constexpr const wchar_t* kLinkCodeFile = L"link-code";
constexpr const wchar_t* kClaimFile = L"claim";
constexpr const wchar_t* kMatchFile = L"match";

// Done: the outcome is written (Finish).
enum class Stage { Setup, Settings, EmberId, Inspect, Approve, Claim, Linked, Match, Room, Ready, Playing, Leaving, Done };

struct Run : Steps<Stage> {
	std::wstring exchange;
	std::string origin, connection, bridge, matchId;
	// The last identity request's ticket. The interface numbers its own from
	// 1, so the test's start far above them.
	std::uint64_t ticket = std::uint64_t(1) << 48;
	bool asked = false, enableSent = false;
	// Reads of the host's fighter's input; see MatchInput.
	unsigned frames = 0;
} s_run;

bool Host() { return Requested() == Test::TournamentHost; }

// The first line of an exchange file, empty until the runner has written it;
// and the game's own, written whole.
std::wstring ExchangePath(const wchar_t* name) { return s_run.exchange + L"\\" + name; }
std::string ReadExchange(const wchar_t* name) { return online::ReadLine(ExchangePath(name)); }
bool WriteExchange(const wchar_t* name, const std::string& line) { return online::WriteLine(ExchangePath(name), line); }

// An identity or bridge request, as the screens send it.
void Ask(IdentityRequest request) {
	request.ticket = ++s_run.ticket;
	RuntimeCommand command;
	command.identity = std::move(request);
	NetplayFacade::SubmitRuntimeCommand(std::move(command));
	s_run.asked = true;
}
IdentityRequest Request(IdentityOp op) {
	IdentityRequest request;
	request.op = op; request.origin = s_run.origin; request.bridge = s_run.bridge; request.connection = s_run.connection;
	return request;
}

// The answer to the last request: none yet, or whether it went through and
// the helper's or the runtime's reason when it did not.
struct Answer { bool given = false, ok = false; std::string reason; };
Answer Answered(const RuntimeSnapshot& now) {
	if (!s_run.asked || now.identityTicket != s_run.ticket) return {};
	if (!now.identityRequest) return {true, false, now.identityRefusal};
	if (now.identity.requestId != now.identityRequest) return {};
	return {true, now.identity.ok, now.identity.failure};
}

// One identity step: the request goes when the step begins and again every
// few seconds until it is answered. True once it went through; a refusal
// fails the step. The request goes before any answer is read: the answer
// to the last step's request is still the newest one on the first tick.
bool Asked(const RuntimeSnapshot& now, IdentityOp op, const std::string& step, Stage otherwise = Stage::Done) {
	if (s_run.Every(10)) { Ask(Request(op)); return false; }
	const auto answer = Answered(now);
	if (answer.given && answer.ok) { s_run.asked = false; return true; }
	if (answer.given) { s_run.asked = false; s_run.Fail(step, answer.reason.empty() ? "refused" : answer.reason, otherwise); return false; }
	if (s_run.Late(60)) s_run.Fail(step, "the helper never answered", otherwise);
	return false;
}

// A tournament command, as the Matches screen sends it.
void SendTournament(netplay::tournament::Command::Op op) {
	RuntimeCommand command;
	command.tournament.op = op; command.tournament.bridgeId = s_run.bridge; command.tournament.matchId = s_run.matchId;
	NetplayFacade::SubmitRuntimeCommand(std::move(command));
}

// The room test's settings, and one round of thirty seconds: a match's room
// is opened from the saved settings, and that ends one game quickly.
netplay::PlayerPreferences TournamentPreferences(const RuntimeSnapshot& now) {
	netplay::PlayerPreferences preferences = online::TestPreferences(now, Host());
	preferences.tableRules.roundTime = 30;
	return preferences;
}

bool Linked(const RuntimeSnapshot& now) {
	for (const auto& link : now.identity.links) if (link.connection == s_run.connection) return true;
	return false;
}

bool Listed(const RuntimeSnapshot& now) {
	const auto& list = now.tournament.list;
	if (list.bridge != s_run.bridge) return false;
	const auto unixNow = static_cast<std::uint64_t>(std::time(nullptr));
	for (const auto& match : list.items) if (match.matchId == s_run.matchId) return match.Playable(unixNow);
	return false;
}

bool Active(Phase phase) { return phase == Phase::Claiming || phase == Phase::Opening || phase == Phase::InRoom; }

void Step(const RuntimeSnapshot& now) {
	Run& run = s_run;
	const auto& id = now.identity;
	const auto phase = now.tournament.phase;
	switch (run.stage) {
	case Stage::Setup:
		if (online::SetupReady(run, now)) run.Enter(Stage::Settings);
		break;
	case Stage::Settings: {
		const auto wanted = TournamentPreferences(now);
		if (now.preferences.displayName == wanted.displayName && now.preferences.tableRules == wanted.tableRules) { Say(true, "settings"); run.Enter(Stage::EmberId); break; }
		if (run.Every(5)) { RuntimeCommand request = Command(netplay::CommandKind::SavePreferences); request.preferences = wanted; request.displayName = wanted.displayName; NetplayFacade::SubmitRuntimeCommand(std::move(request)); }
		if (run.Late(20)) run.Fail("settings", "were not taken");
		break;
	}
	case Stage::EmberId:
		// The self-test's own Ember folder starts empty, so the ID is made
		// here, as Create does on the Ember ID screen. One that is ready before
		// Enable was sent is somebody else's: the helper did not use the test's
		// folder, and the test would trust the bridge in the player's own.
		if (id.state == "ready" && !run.enableSent) {
			run.Fail("ember id", "the helper is not using the test's Ember folder (it found an ID there before the test made one)");
			break;
		}
		if (id.state == "ready") {
			if (!WriteExchange(kEmberIdFile, id.emberId + " " + sidecarHash)) { run.Fail("ember id", "could not be written to the exchange folder"); break; }
			Say(true, "ember id", id.emberId);
			run.Enter(Stage::Inspect);
		}
		else if (id.passphraseRequired) run.Fail("ember id", "this PC needs a passphrase for it (Wine or Proton)");
		else if (id.state == "disabled" && !run.enableSent) { Ask(Request(IdentityOp::Enable)); run.enableSent = true; }
		else if (run.Every(5)) Ask(Request(IdentityOp::Status));
		if (run.stage == Stage::EmberId && run.Late(60))
			run.Fail("ember id", !now.identityRefusal.empty() ? now.identityRefusal : id.state.empty() ? "no answer" : id.state + " " + id.reason);
		break;
	case Stage::Inspect:
		// Look up, as the Linked accounts screen does with a typed address.
		if (!Asked(now, IdentityOp::BridgeInspect, "bridge lookup")) break;
		if (id.inspected.id.empty() || id.inspected.origin != run.origin) { run.Fail("bridge lookup", "the service at " + run.origin + " named another"); break; }
		run.bridge = id.inspected.id;
		{
			bool offered = false;
			for (const auto& connection : id.connections) offered = offered || connection.id == run.connection;
			if (!offered) { run.Fail("bridge lookup", "the service offers no connection " + run.connection); break; }
		}
		run.Enter(Stage::Approve);
		break;
	case Stage::Approve:
		// Trust, which the player confirms on that screen.
		if (!Asked(now, IdentityOp::BridgeApprove, "bridge trust")) break;
		Say(true, "bridge trusted", run.bridge);
		run.Enter(Stage::Claim);
		break;
	case Stage::Claim: {
		// The code the runner's provider made for this player, typed into Link
		// code. Its fingerprint goes back for the runner to compare before it
		// approves the claim, as the player compares the two on their screens.
		if (!run.asked) {
			const std::string code = ReadExchange(kLinkCodeFile);
			if (code.empty()) { if (run.Late(300)) run.Fail("link", "no link code arrived from the runner"); break; }
			IdentityRequest request = Request(IdentityOp::LinkClaim);
			request.code = code;
			Ask(std::move(request));
			break;
		}
		const auto answer = Answered(now);
		if (!answer.given) { if (run.Late(360)) run.Fail("link", "the claim was never answered"); break; }
		run.asked = false;
		if (!answer.ok || id.claimFingerprint.empty()) { run.Fail("link", answer.reason.empty() ? "no fingerprint" : answer.reason); break; }
		if (!WriteExchange(kClaimFile, id.claimFingerprint)) { run.Fail("link", "the claim could not be written to the exchange folder"); break; }
		Say(true, "link claimed", id.claimFingerprint);
		run.Enter(Stage::Linked);
		break;
	}
	case Stage::Linked:
		// The runner approves the claim; the link then shows in the list.
		if (Linked(now)) { Say(true, "linked", run.connection); run.Enter(Stage::Match); break; }
		if (run.Every(3)) Ask(Request(IdentityOp::LinkList));
		if (run.Late(180)) run.Fail("linked", "the claim was never approved");
		break;
	case Stage::Match:
		// The runner creates the match once both players are linked; it is
		// played from the assignment list, as the Matches screen plays it.
		if (run.matchId.empty()) {
			run.matchId = ReadExchange(kMatchFile);
			if (run.matchId.empty() && run.Late(300)) run.Fail("match", "no match arrived from the runner");
			break;
		}
		if (Listed(now)) {
			Say(true, "match listed", run.matchId);
			SendTournament(netplay::tournament::Command::Op::Play);
			run.Enter(Stage::Room);
			break;
		}
		if (run.Every(5)) SendTournament(netplay::tournament::Command::Op::Refresh);
		if (run.Late(120)) run.Fail("match", now.tournament.list.error.empty() ? "never listed as playable" : now.tournament.list.error);
		break;
	case Stage::Room:
		// Claim, host or join, publish and bind are the runtime's own; the
		// bound room seats each fighter at the match's table.
		if (phase == Phase::InRoom && now.session.room == netplay::RoomState::Joined && now.localSlot >= 0) {
			Say(true, "match room", "Player " + std::to_string(now.localSlot + 1));
			run.Enter(Stage::Ready);
		}
		else if (phase == Phase::Failed) run.Fail("match room", now.tournament.reason, Stage::Leaving);
		else if (run.Late(180)) run.Fail("match room", "never seated", Stage::Leaving);
		break;
	case Stage::Ready: {
		// Ready as Ryu, as the room test does, until the bridge's permit has
		// started the game.
		if (now.session.match == netplay::MatchState::Playing) { Say(true, "match live"); run.Enter(Stage::Playing); break; }
		if (phase == Phase::Failed) { run.Fail("match live", now.tournament.reason, Stage::Leaving); break; }
		if (run.Every(5)) online::ReadyAsRyu(now, room::TournamentTable);
		if (run.Late(180)) run.Fail("match live", now.readyFailure.empty() ? "never went live" : now.readyFailure, Stage::Leaving);
		break;
	}
	case Stage::Playing:
		// The game ends in the host's win (MatchInput); both report it, and
		// the bridge's answer to the next claim says the match is over.
		if (phase == Phase::Finished) { Say(true, "match finished", now.tournament.reason); run.Enter(Stage::Leaving); }
		else if (phase == Phase::Failed) run.Fail("match finished", now.tournament.reason, Stage::Leaving);
		else if (run.Late(360)) run.Fail("match finished", "the match never ended", Stage::Leaving);
		break;
	case Stage::Leaving:
		// A finished match's room is left once its last battle has closed; a
		// failed one is stopped, as Stop on the Matches screen does.
		if (now.session.room == netplay::RoomState::Idle && !Active(phase)) { Say(true, "left the room"); run.Enter(Stage::Done); Finish(); break; }
		if (Active(phase) && run.Every(5)) SendTournament(netplay::tournament::Command::Op::Stop);
		if (run.Late(150)) run.Fail("leave", "the room never let go");
		break;
	case Stage::Done:
		break;
	}
}

}

bool StartTournament() {
	s_run.exchange = Environment(L"SF4E_SELFTEST_EXCHANGE");
	s_run.origin = platform::WideToUtf8(Environment(L"SF4E_SELFTEST_BRIDGE"));
	s_run.connection = platform::WideToUtf8(Environment(L"SF4E_SELFTEST_CONNECTION"));
	if (s_run.exchange.empty() || s_run.origin.empty() || s_run.connection.empty()) {
		Say(false, "tournament", "SF4E_SELFTEST_EXCHANGE, SF4E_SELFTEST_BRIDGE and SF4E_SELFTEST_CONNECTION are needed");
		return false;
	}
	s_run.Enter(Stage::Setup);
	return true;
}

void TickTournament() {
	s_run.waited++;
	Step(*Now());
}

void MatchInput(int side, unsigned& mapped, unsigned& raw) {
	if (Requested() != Test::TournamentHost && Requested() != Test::TournamentGuest) return;
	// The guest's fighter stands still, whatever the keyboard does; the host's
	// attacks once the game is live, from the side it starts on.
	const bool attacks = Host() && s_run.stage == Stage::Playing;
	mapped = raw = attacks ? AttackInput(s_run.frames++, side == 0) : 0u;
}

} }
