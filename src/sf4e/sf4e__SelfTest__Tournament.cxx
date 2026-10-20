#include "sf4e__SelfTest__Online.hxx"

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "../Dimps/Dimps__Game__Battle.hxx"
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
//
// With SF4E_SELFTEST_SITE (the Challonge mode) the account is not linked by a
// code. The test sends the command the site's Connect screen sends, which opens
// the site's sign-in page in this PC's browser, where a person signs in and
// approves, and reads the account until it shows connected. The connector then
// finds the player by that account, links it and makes the match, and the test
// plays the one match the bridge lists.
namespace sf4e { namespace selftest {
namespace {

using NetplayFacade::RuntimeCommand;
using NetplayFacade::RuntimeSnapshot;
using netplay::IdentityOp;
using netplay::IdentityRequest;
using netplay::tournament::Phase;
using netplay::tournament::PlayUnderWay;
using online::Command;
using online::Now;

// The files of the exchange folder. The game writes its Ember ID and build,
// then its claim's fingerprint; the runner writes the link code, then the
// match. In the Challonge mode the game writes the account its sign-in
// connected, then the match it plays.
constexpr const wchar_t* kEmberIdFile = L"ember-id";
constexpr const wchar_t* kLinkCodeFile = L"link-code";
constexpr const wchar_t* kClaimFile = L"claim";
constexpr const wchar_t* kMatchFile = L"match";
constexpr const wchar_t* kSiteAccountFile = L"site-account";
constexpr const wchar_t* kMatchIdFile = L"match-id";
// The desk run (docs/guides/TOURNAMENT_TEST.md): the runner writes the code
// the web desk shows and then the number it shows; the organizer's game
// writes what asked and the numbers it offers, then that it approved. The
// runner writes the event to sign up for; each game writes that it did.
constexpr const wchar_t* kDeskCodeFile = L"desk-code";
constexpr const wchar_t* kDeskDeviceFile = L"desk-device";
constexpr const wchar_t* kDeskNumberFile = L"desk-number";
constexpr const wchar_t* kDeskApprovedFile = L"desk-approved";
constexpr const wchar_t* kEventFile = L"event";
constexpr const wchar_t* kSignedUpFile = L"signed-up";

// A person has this long to sign in and approve in the browser, on each PC in
// turn. The connector links the players only once both have, so the wait for
// the link is longer than that.
constexpr int kSiteSignInSeconds = 300, kSiteLinkSeconds = 420;

// Done: the outcome is written (Finish).
enum class Stage { Setup, Settings, EmberId, Inspect, Approve, SiteRead, SiteOpen, SiteWait, OrganizerRead, OrganizerOpen, OrganizerWait,
	DeskCode, DeskNumber, SignUp, Claim, Linked, Match, Room, Ready, Playing, Leaving, Done };

// How a player is linked and finds the match, which is the one thing the two
// modes do differently. It is chosen once, from SF4E_SELFTEST_SITE, so the
// steps read what the mode does and never ask which mode it is.
struct Flow {
	// The sign-in site to connect an account on (the Challonge mode), empty
	// for the code link.
	std::string site;
	// The stage that follows trusting the bridge: the claim of a link code,
	// or reading the account on the site before the sign-in.
	Stage afterApprove = Stage::Claim;
	// How long the link may take to show, and what its failure says.
	int linkSeconds = 180;
	const char* linkFailure = "the claim was never approved";
	// Whether the runner creates the match and names it (the code link), or
	// the bracket connector makes it and the test plays the one the bridge
	// lists as playable and names it to the runner.
	bool runnerNamesMatch = true;
	// The desk run, against a running bridge such as staging: the Ember ID is
	// kept between runs in a folder of the test's own (its accounts connect
	// once, by a person), this game is the organizer who approves the web
	// desk's code with its number, each game signs up for the event, and the
	// first game is a draw.
	bool kept = false, organizer = false, signUp = false, drawFirst = false;
	// Rematch sent after the drawn game, once.
	bool rematched = false;

	bool SignIn() const { return !site.empty(); }
};

Flow CodeLink() { return {}; }
Flow SiteLink(std::string site) {
	Flow flow;
	flow.site = std::move(site);
	flow.afterApprove = Stage::SiteRead;
	flow.linkSeconds = kSiteLinkSeconds;
	flow.linkFailure = "the connector never linked this account";
	flow.runnerNamesMatch = false;
	return flow;
}

struct Run : Steps<Stage> {
	std::wstring exchange;
	std::string origin, connection, bridge, matchId;
	Flow flow;
	// The account on the site before the sign-in, and the last refusal of a
	// read of it, for the message of a wait that ran out.
	std::string lastRefusal;
	netplay::IdentityAccount accountBefore;
	// The last identity request's ticket. The interface numbers its own from
	// 1, so the test's start far above them.
	std::uint64_t ticket = std::uint64_t(1) << 48;
	bool asked = false, enableSent = false;
	// Reads of the host's fighter's input; see MatchInput.
	unsigned frames = 0;
} s_run;

bool Host() { return Requested() == Test::TournamentHost; }

// What follows a connected account: organizer access and the desk's code,
// then the sign-up, then the link the connector makes.
Stage AfterDesk() { return s_run.flow.signUp ? Stage::SignUp : Stage::Linked; }
Stage AfterSite() { return s_run.flow.organizer ? Stage::OrganizerRead : AfterDesk(); }

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
	request.site = s_run.flow.site;
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

// The matches the bridge lists for this player that can be played now.
std::vector<std::string> PlayableMatches(const RuntimeSnapshot& now) {
	std::vector<std::string> playable;
	const auto& list = now.tournament.list;
	if (list.bridge != s_run.bridge) return playable;
	const auto unixNow = static_cast<std::uint64_t>(std::time(nullptr));
	for (const auto& match : list.items) if (match.Playable(unixNow)) playable.push_back(match.matchId);
	return playable;
}

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
		if (id.state == "ready" && !run.enableSent && !run.flow.kept) {
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
		if (run.flow.SignIn()) {
			bool signIn = false;
			for (const auto& site : id.signInSites) signIn = signIn || (site.id == run.flow.site && site.signIn);
			if (!signIn) { run.Fail("bridge lookup", "the service offers no sign-in on " + run.flow.site); break; }
		}
		run.Enter(Stage::Approve);
		break;
	case Stage::Approve:
		// Trust, which the player confirms on that screen.
		if (!Asked(now, IdentityOp::BridgeApprove, "bridge trust")) break;
		Say(true, "bridge trusted", run.bridge);
		run.Enter(run.flow.afterApprove);
		break;
	case Stage::SiteRead:
		// The account on the site before the sign-in, as the Connect screen
		// reads it, so that the one that appears is known to be new.
		if (!Asked(now, IdentityOp::SiteStatus, "site account")) break;
		run.accountBefore = id.Site();
		// A kept Ember ID connected its account on an earlier run.
		if (run.flow.kept && !id.Site().user.empty()) {
			const auto account = id.Site();
			if (!WriteExchange(kSiteAccountFile, account.user + " " + (account.name.empty() ? account.user : account.name))) { run.Fail("site connected", "the account could not be written to the exchange folder"); break; }
			Say(true, "site connected", "kept");
			run.Enter(AfterSite());
			break;
		}
		run.Enter(Stage::SiteOpen);
		break;
	case Stage::SiteOpen: {
		// Connect, as the site's Connect screen sends it: the helper starts a
		// sign-in on the bridge and opens the site's page in this PC's browser.
		// Sent once, since each one starts a sign-in.
		if (!run.asked) { Ask(Request(IdentityOp::SiteConnect)); break; }
		const auto answer = Answered(now);
		if (!answer.given) { if (run.Late(60)) run.Fail("site sign-in", "the helper never opened the page"); break; }
		run.asked = false;
		if (!answer.ok) { run.Fail("site sign-in", answer.reason.empty() ? "refused" : answer.reason); break; }
		Say(true, "site sign-in opened", run.flow.site);
		run.Enter(Stage::SiteWait);
		break;
	}
	case Stage::SiteWait: {
		// A person signs in and approves in the browser; the account shows on
		// the bridge a moment later. It is read every few seconds, as the
		// Connect screen does, until an account other than the earlier one is
		// there. A read that is not answered is asked again after a while.
		if (run.asked) {
			const auto answer = Answered(now);
			if (answer.given) {
				run.asked = false;
				if (!answer.ok) run.lastRefusal = answer.reason;
				else if (id.Site().ConnectedSince(run.accountBefore)) {
					const auto account = id.Site();
					// The account goes to the runner in the exchange folder and no
					// further: the line the game logs does not name it.
					const std::string name = account.name.empty() ? account.user : account.name;
					if (!WriteExchange(kSiteAccountFile, account.user + " " + name)) { run.Fail("site connected", "the account could not be written to the exchange folder"); break; }
					Say(true, "site connected");
					run.Enter(AfterSite());
					break;
				}
			}
		}
		if (run.Every(4) && (!run.asked || run.Every(12))) Ask(Request(IdentityOp::SiteStatus));
		if (run.Late(kSiteSignInSeconds))
			run.Fail("site connected", "no account was connected in " + std::to_string(kSiteSignInSeconds / 60) + " minutes" + (run.lastRefusal.empty() ? "" : " (" + run.lastRefusal + ")"));
		break;
	}
	case Stage::OrganizerRead: case Stage::OrganizerWait: {
		// Organizer access, as Tournament organizer reads it; a kept Ember ID
		// holds it from an earlier run. Without it, Allow opens the site's
		// organizer sign-in once and a person approves.
		if (run.asked) {
			const auto answer = Answered(now);
			if (answer.given) {
				run.asked = false;
				if (answer.ok && id.siteOrganizer) { Say(true, "organizer access", run.flow.site); run.Enter(Stage::DeskCode); break; }
				if (run.stage == Stage::OrganizerRead) { run.Enter(Stage::OrganizerOpen); break; }
			}
		}
		if (!run.asked && run.Every(4)) { IdentityRequest read = Request(IdentityOp::SiteStatus); read.organizer = true; Ask(std::move(read)); }
		if (run.Late(run.stage == Stage::OrganizerRead ? 60 : kSiteSignInSeconds)) run.Fail("organizer access", "was never allowed");
		break;
	}
	case Stage::OrganizerOpen: {
		if (!run.asked) { IdentityRequest open = Request(IdentityOp::SiteConnect); open.organizer = true; Ask(std::move(open)); break; }
		const auto answer = Answered(now);
		if (!answer.given) { if (run.Late(60)) run.Fail("organizer sign-in", "the helper never opened the page"); break; }
		run.asked = false;
		if (!answer.ok) { run.Fail("organizer sign-in", answer.reason.empty() ? "refused" : answer.reason); break; }
		Say(true, "organizer sign-in opened", run.flow.site);
		run.Enter(Stage::OrganizerWait);
		break;
	}
	case Stage::DeskCode: {
		// The code the web desk shows, typed on Sign in on another device:
		// read first, as that screen does, for what asked and the numbers.
		if (!run.asked) {
			const std::string code = ReadExchange(kDeskCodeFile);
			if (code.empty()) { if (run.Late(600)) run.Fail("desk code", "no code arrived from the runner"); break; }
			IdentityRequest info = Request(IdentityOp::DeskCodeInfo);
			info.command = code;
			Ask(std::move(info));
			break;
		}
		const auto answer = Answered(now);
		if (!answer.given) { if (run.Late(60)) run.Fail("desk code", "the helper never answered"); break; }
		run.asked = false;
		if (!answer.ok || id.deskDevice.choices.empty()) { run.Fail("desk code", answer.reason.empty() ? "no numbers to pick from" : answer.reason); break; }
		std::string choices;
		for (const int choice : id.deskDevice.choices) choices += (choices.empty() ? "" : ",") + std::to_string(choice);
		if (!WriteExchange(kDeskDeviceFile, choices + " " + id.deskDevice.device)) { run.Fail("desk code", "could not be written to the exchange folder"); break; }
		Say(true, "desk code read", id.deskDevice.device);
		run.Enter(Stage::DeskNumber);
		break;
	}
	case Stage::DeskNumber: {
		// The number the person sees on the other device, picked from three.
		if (!run.asked) {
			const std::string number = ReadExchange(kDeskNumberFile);
			if (number.empty()) { if (run.Late(120)) run.Fail("desk approve", "no number arrived from the runner"); break; }
			IdentityRequest approve = Request(IdentityOp::DeskApprove);
			approve.command = ReadExchange(kDeskCodeFile);
			approve.number = static_cast<std::uint8_t>(std::atoi(number.c_str()));
			Ask(std::move(approve));
			break;
		}
		const auto answer = Answered(now);
		if (!answer.given) { if (run.Late(60)) run.Fail("desk approve", "the helper never answered"); break; }
		run.asked = false;
		if (!answer.ok) { run.Fail("desk approve", answer.reason.empty() ? "refused" : answer.reason); break; }
		if (!WriteExchange(kDeskApprovedFile, "approved")) { run.Fail("desk approve", "could not be written to the exchange folder"); break; }
		Say(true, "desk approved");
		run.Enter(AfterDesk());
		break;
	}
	case Stage::SignUp: {
		// Sign up for the event the runner names, as the event's Sign up does.
		if (!run.asked) {
			const std::string event = ReadExchange(kEventFile);
			if (event.empty()) { if (run.Late(900)) run.Fail("sign up", "no event arrived from the runner"); break; }
			IdentityRequest signUp = Request(IdentityOp::DeskAct);
			signUp.target = "/v1/events/" + event + "/sign-up";
			signUp.command = "{}";
			Ask(std::move(signUp));
			break;
		}
		const auto answer = Answered(now);
		if (!answer.given) { if (run.Late(60)) run.Fail("sign up", "the helper never answered"); break; }
		run.asked = false;
		if (!answer.ok) { run.Fail("sign up", answer.reason.empty() ? "refused" : answer.reason); break; }
		if (!WriteExchange(kSignedUpFile, "signed up")) { run.Fail("sign up", "could not be written to the exchange folder"); break; }
		Say(true, "signed up");
		run.Enter(Stage::Linked);
		break;
	}
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
		// The runner approves the claim, or in the Challonge mode the connector
		// finds the account and links it; the link then shows in the list.
		if (Linked(now)) { Say(true, "linked", run.connection); run.Enter(Stage::Match); break; }
		if (run.Every(3)) Ask(Request(IdentityOp::LinkList));
		if (run.Late(run.flow.linkSeconds)) run.Fail("linked", run.flow.linkFailure);
		break;
	case Stage::Match:
		// The runner creates the match once both players are linked; it is
		// played from the assignment list, as the Matches screen plays it.
		if (run.matchId.empty() && run.flow.runnerNamesMatch) {
			run.matchId = ReadExchange(kMatchFile);
			if (run.matchId.empty() && run.Late(300)) run.Fail("match", "no match arrived from the runner");
			break;
		}
		if (run.matchId.empty()) {
			// The connector made the match, so the test plays the one the bridge
			// lists as playable, and tells the runner which it is.
			const auto playable = PlayableMatches(now);
			if (playable.size() > 1) { run.Fail("match", "the bridge lists " + std::to_string(playable.size()) + " playable matches, not one"); break; }
			if (playable.size() == 1) {
				run.matchId = playable.front();
				if (!WriteExchange(kMatchIdFile, run.matchId)) { run.Fail("match", "the match could not be written to the exchange folder"); break; }
			}
		}
		if (!run.matchId.empty() && Listed(now)) {
			Say(true, "match listed", run.matchId);
			SendTournament(netplay::tournament::Command::Op::Play);
			run.Enter(Stage::Room);
			break;
		}
		if (run.Every(5)) SendTournament(netplay::tournament::Command::Op::Refresh);
		// The desk run's connector reads the bracket every few minutes.
		if (run.Late(run.flow.signUp ? 600 : 120)) run.Fail("match", now.tournament.list.error.empty() ? "never listed as playable" : now.tournament.list.error);
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
		// the bridge's answer to the next claim says the match is over. A
		// drawn game is played again once both players send Rematch, as
		// Rematch on the room's screen does.
		if (phase == Phase::Finished) { Say(true, "match finished", now.tournament.reason); run.Enter(Stage::Leaving); }
		else if (run.flow.drawFirst && !run.flow.rematched && now.session.match == netplay::MatchState::PostMatch && !now.session.readyPending) {
			RuntimeCommand rematch = Command(netplay::CommandKind::Rematch);
			rematch.character.charaID = 0; rematch.character.unc_edition = Dimps::Game::Battle::ED_USF4;
			NetplayFacade::SubmitRuntimeCommand(std::move(rematch));
			run.flow.rematched = true;
			Say(true, "drawn game, rematch");
		}
		else if (phase == Phase::Failed) run.Fail("match finished", now.tournament.reason, Stage::Leaving);
		else if (run.Late(360)) run.Fail("match finished", "the match never ended", Stage::Leaving);
		break;
	case Stage::Leaving:
		// A finished match's room is left once its last battle has closed; a
		// failed one is stopped, as Stop on the Matches screen does.
		if (now.session.room == netplay::RoomState::Idle && !PlayUnderWay(phase)) { Say(true, "left the room"); run.Enter(Stage::Done); Finish(); break; }
		if (PlayUnderWay(phase) && run.Every(5)) SendTournament(netplay::tournament::Command::Op::Stop);
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
	// Optional: the sign-in site to connect an account on instead of a code.
	const std::string site = platform::WideToUtf8(Environment(L"SF4E_SELFTEST_SITE"));
	s_run.flow = site.empty() ? CodeLink() : SiteLink(site);
	const auto on = [](const wchar_t* name) { return Environment(name) == L"1"; };
	s_run.flow.kept = on(L"SF4E_SELFTEST_EMBER_KEPT");
	s_run.flow.organizer = on(L"SF4E_SELFTEST_ORGANIZER");
	s_run.flow.signUp = on(L"SF4E_SELFTEST_SIGN_UP");
	s_run.flow.drawFirst = on(L"SF4E_SELFTEST_DRAW_FIRST");
	// The organizer readies the event and starts its bracket after both have
	// signed up, and the connector links players on its next read.
	if (s_run.flow.signUp) s_run.flow.linkSeconds = 900;
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
	// With a draw first, the host stands still until the first game has
	// ended on time (one round of 30 seconds) and the next has begun.
	// ponytail: by time, not by game; count games if a round's length changes.
	const bool attacks = Host() && s_run.stage == Stage::Playing && (!s_run.flow.drawFirst || s_run.Late(50));
	mapped = raw = attacks ? AttackInput(s_run.frames++, side == 0) : 0u;
}

} }
