#include "../sf4e/sf4e.hxx"
#include "../sf4e/sf4e__NetplayFacade.hxx"
#include "../Dimps/Dimps.hxx"
#include <cstdlib>
#include <iostream>
#include <type_traits>

#define CHECK(c) do { if (!(c)) { std::cerr << "Check failed at " << __LINE__ << ": " #c << '\n'; std::exit(1); } } while (false)
using namespace sf4e;

static unsigned rootQueries = 0;
static Dimps::GameEvents::RootEvent* UnavailableRoot() { ++rootQueries; return nullptr; }

int wmain(int argc, wchar_t** argv) {
	CHECK(argc == 2);
	static_assert(std::is_trivially_copyable<Payload>::value, "Injected bootstrap must remain POD-copyable");
	Payload payload = {};
	CHECK(IsCompatiblePayload(&payload, sizeof(payload)));
	CHECK(!IsCompatiblePayload(nullptr, sizeof(payload)));
	CHECK(!IsCompatiblePayload(&payload, sizeof(payload) - 1));
	payload.magic = 0;
	CHECK(!IsCompatiblePayload(&payload, sizeof(payload)));
	payload = {};
	payload.version = 0;
	CHECK(!IsCompatiblePayload(&payload, sizeof(payload)));

	NetplayConfig config = {};
	NetplayFacade::InitFromPayload(config);
	NetplayFacade::ConfigureHelper({}, ERROR_FILE_NOT_FOUND);
	NetplayFacade::StartHelper();
	const auto originalRootGetter = Dimps::App::GetRootEvent;
	Dimps::App::GetRootEvent = UnavailableRoot;
	// Platform initialization finishes before the native root-event getter is
	// safe to invoke. Returning nullptr from a stub exposes even an attempted
	// early call without crashing the test process inside the game executable.
	NetplayFacade::NotifyRuntimeGameReady();
	NetplayFacade::TickRuntime();
	CHECK(rootQueries == 0);
	NetplayFacade::NotifyRuntimeEventSystemReady();
	NetplayFacade::TickRuntime();
	CHECK(rootQueries > 0);
	CHECK(!NetplayFacade::GetRuntimeSnapshot().atMainMenu);
	auto snapshot = NetplayFacade::GetRuntimeSnapshot();
	CHECK(!snapshot.helperReady && !snapshot.helperError.empty());
	CHECK(!snapshot.canEditPreferences && !snapshot.canEditLobby);
	// Exercise the real UI-to-runtime admission seam: these commands have
	// handlers, even though the session controller rejects them outside a room.
	const auto initialDelay = snapshot.selectedDelay;
	for (auto kind : {netplay::CommandKind::CheckConnection, netplay::CommandKind::ApplyDelay,
		netplay::CommandKind::ReplaceRoom}) {
		NetplayFacade::RuntimeCommand action;
		action.command = {kind, snapshot.session.generation, {}};
		action.selectedDelay = 4;
		CHECK(NetplayFacade::SubmitRuntimeCommand(action));
		NetplayFacade::TickRuntime();
		CHECK(NetplayFacade::GetRuntimeSnapshot().session.room == netplay::RoomState::Idle);
		CHECK(NetplayFacade::GetRuntimeSnapshot().selectedDelay == initialDelay);
	}
	NetplayFacade::RuntimeCommand preferences;
	preferences.command = {netplay::CommandKind::SavePreferences, snapshot.session.generation, {}};
	preferences.preferences.displayName = "Must wait for the menu";
	CHECK(NetplayFacade::SubmitRuntimeCommand(preferences));
	preferences.command.kind = netplay::CommandKind::SetLobbySettings;
	CHECK(NetplayFacade::SubmitRuntimeCommand(preferences));
	NetplayFacade::TickRuntime();
	CHECK(NetplayFacade::GetRuntimeSnapshot().preferences.displayName == "Player");
	CHECK(NetplayFacade::SubmitRuntimeCommand({{netplay::CommandKind::StartOffline, snapshot.session.generation, {}}, {}}));
	NetplayFacade::TickRuntime();
	CHECK(NetplayFacade::GetRuntimeSnapshot().offlineRequested);
	NetplayFacade::StopHelper();
	Dimps::App::GetRootEvent = originalRootGetter;

	platform::HelperProcess process;
	CHECK(process.Start(argv[1], GetCurrentProcessId()));
	auto bootstrap = process.Bootstrap();
	NetplayFacade::ConfigureHelper(bootstrap, 0);
	SecureZeroMemory(bootstrap.nonce, sizeof(bootstrap.nonce));
	NetplayFacade::StartHelper();
	const auto deadline = GetTickCount64() + 15000;
	do {
		NetplayFacade::TickRuntime();
		snapshot = NetplayFacade::GetRuntimeSnapshot();
		if (snapshot.helperReady) break;
		Sleep(1);
	} while (GetTickCount64() < deadline);
	CHECK(snapshot.helperReady);
	CHECK(!snapshot.canOpenRoom && !snapshot.atMainMenu);
	// No game is loaded: commands must not touch Dimps addresses before readiness.
	NetplayFacade::RuntimeCommand host;
	host.command = {netplay::CommandKind::HostRoom, snapshot.session.generation, {}};
	host.displayName = "Host";
	CHECK(NetplayFacade::SubmitRuntimeCommand(std::move(host)));
	NetplayFacade::TickRuntime();
	CHECK(NetplayFacade::GetRuntimeSnapshot().session.room == netplay::RoomState::Idle);
	NetplayFacade::StopHelper();
	const auto stopDeadline = GetTickCount64() + 3000;
	while (process.IsRunning() && GetTickCount64() < stopDeadline) Sleep(1);
	CHECK(!process.IsRunning());
	std::cout << "Runtime bootstrap, deferred authentication, offline recovery, and shutdown passed\n";
	return 0;
}
