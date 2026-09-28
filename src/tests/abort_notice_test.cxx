// An abort's reason must reach the player. AbortGgpoMatch published it and
// then retired the session, whose teardown clears the match notice, so every
// abort explanation (the spectator's "stream was dropped" among them) was
// wiped in the same call. The reason is now published after the retirement,
// and cleared when the next session starts.
#include "../sf4e/sf4e__Game__Battle__System.hxx"
#include "../sf4e/sf4e__NetplayFacade.hxx"
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

	// A notice raised during the session ends with it, as before.
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

	std::cout << "Abort notice passed\n";
	return 0;
}
