// A notice that says why a match ended must reach the player. Retiring the
// session clears its notices, so AbortGgpoMatch publishes the reason after the
// retirement. An Error also survives a retirement that is not an abort (the
// opponent disconnected, the stream dropped, a desync, a failed room), and
// stays until a newer notice replaces it or the next session starts. Info and
// Warning notices end with their session.
#include "../sf4e/sf4e__Game__Battle__System.hxx"
#include "../sf4e/sf4e__NetplayFacade.hxx"
#include "../common/Localization.hxx"
#include <winsock2.h>
#include <cstring>
#include <iostream>

#include "test_support.hxx"

namespace facade = sf4e::NetplayFacade;
using fSystem = sf4e::Game::Battle::System;

static bool __cdecl Begin(const char*) { return true; }
static bool __cdecl Save(unsigned char**, int*, int*, int) { return false; }
static bool __cdecl Load(unsigned char*, int) { return true; }
static bool __cdecl Log(char*, unsigned char*, int) { return true; }
static void __cdecl Free(void*) {}
static bool __cdecl Advance(int) { return true; }
static bool __cdecl Event(GGPOEvent*) { return true; }

// A live spectator session towards a port nobody answers: enough for
// RetireGgpoSession to have a session to close.
static void StartSession() {
	GGPOSessionCallbacks callbacks = {};
	callbacks.begin_game = Begin; callbacks.save_game_state = Save; callbacks.load_game_state = Load;
	callbacks.log_game_state = Log; callbacks.free_buffer = Free; callbacks.advance_frame = Advance; callbacks.on_event = Event;
	char host[] = "127.0.0.1";
	CHECK(ggpo_start_spectating(&fSystem::ggpo, &callbacks, "abort-notice", 2, 2, 0, host, 9) == GGPO_OK);
	CHECK(fSystem::ggpo != nullptr);
}

static std::string Notice(sf4e::NoticeSeverity* severity = nullptr) {
	const auto status = facade::GetStatus();
	if (severity) *severity = status.lastErrorSeverity;
	return status.lastError;
}

int main() {
	WSADATA winsock{}; CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);

	// A notice raised during the session ends with it.
	StartSession();
	facade::PushAlert("Connection unstable.", sf4e::NoticeSeverity::Warning);
	fSystem::RetireGgpoSession("test");
	CHECK(!fSystem::ggpo);
	CHECK(Notice().empty());

	// The abort's own reason survives the retirement it causes.
	StartSession();
	fSystem::AbortGgpoMatch("The spectator stream was dropped.");
	CHECK(!fSystem::ggpo);
	sf4e::NoticeSeverity severity = sf4e::NoticeSeverity::Info;
	CHECK(Notice(&severity) == "The spectator stream was dropped." && severity == sf4e::NoticeSeverity::Error);

	// A player's own choice to leave is a passing note, not an error.
	StartSession();
	fSystem::AbortGgpoMatch("Returning to the room.", sf4e::NoticeSeverity::Info);
	CHECK(Notice(&severity) == "Returning to the room." && severity == sf4e::NoticeSeverity::Info);

	// A quiet abort (the result was already recorded) publishes nothing and
	// leaves no stale notice from the session behind.
	StartSession();
	facade::PushAlert("Connection unstable.", sf4e::NoticeSeverity::Warning);
	fSystem::AbortGgpoMatch("");
	CHECK(!fSystem::ggpo);
	CHECK(Notice().empty());

	// An Error raised while the session was live (the battle-close path of a
	// GGPO disconnect) outlives the retirement, so the player still reads why
	// the match ended once the room shell is back.
	for (const char* label : {"battle_close", "deferred_close"}) {
		facade::ClearMatchNotice();
		StartSession();
		facade::PushAlert("Opponent disconnected. The match is over.", sf4e::NoticeSeverity::Error);
		fSystem::RetireGgpoSession(label);
		CHECK(!fSystem::ggpo);
		CHECK(Notice(&severity) == "Opponent disconnected. The match is over." && severity == sf4e::NoticeSeverity::Error);
		// A newer notice still replaces it.
		facade::PushAlert("Returning to the room.", sf4e::NoticeSeverity::Info);
		CHECK(Notice(&severity) == "Returning to the room." && severity == sf4e::NoticeSeverity::Info);
	}

	// Shutting the netplay session down keeps the Error too, which is what the
	// failure reason handed to HandleNetplayFailure relies on.
	facade::ClearMatchNotice();
	StartSession();
	facade::HandleNetplayFailure("The room was lost.", true);
	CHECK(!fSystem::ggpo);
	CHECK(Notice(&severity) == "The room was lost." && severity == sf4e::NoticeSeverity::Error);
	// ...but a passing note does not follow it out.
	facade::ClearMatchNotice();
	StartSession();
	facade::PushAlert("Connection unstable.", sf4e::NoticeSeverity::Warning);
	facade::ShutdownNetplay(true);
	CHECK(!fSystem::ggpo);
	CHECK(Notice().empty());

	// The full clear, used when a new session starts, wipes an Error.
	facade::PushAlert("Old failure.", sf4e::NoticeSeverity::Error);
	facade::ClearMatchNotice();
	CHECK(Notice().empty());

	// A desync is announced from the catalog in the active language, as an Error
	// when the fight ended and as a Warning when only a spectator's view did.
	sf4e::loc::SetActive(sf4e::loc::Locale::Fr);
	facade::PushDesyncNotice(false);
	CHECK(Notice(&severity) == sf4e::loc::T("runtime.desync_match_ended") && severity == sf4e::NoticeSeverity::Error);
	CHECK(Notice() != "Match ended: the two games diverged (desync). Export diagnostics from both players.");
	facade::PushDesyncNotice(true);
	CHECK(Notice(&severity) == sf4e::loc::T("runtime.desync_spectator") && severity == sf4e::NoticeSeverity::Warning);
	sf4e::loc::SetActive(sf4e::loc::Locale::En);
	facade::ClearMatchNotice();

	// The overlay reads the reason from the frame the tick publishes.
	StartSession();
	fSystem::AbortGgpoMatch("The spectator stream was dropped.");
	facade::PublishPresentationFrame();
	const auto frame = facade::GetPresentationSnapshotShared();
	CHECK(std::string(frame->netplay.lastError) == "The spectator stream was dropped.");
	CHECK(frame->netplay.lastErrorSeverity == sf4e::NoticeSeverity::Error && !frame->ggpoSessionActive);
	facade::ClearMatchNotice();

	std::cout << "Abort notice passed\n";
	return 0;
}
