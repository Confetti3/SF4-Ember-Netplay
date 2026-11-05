#include "sf4e__RuntimeTick.hxx"

#include "../common/Localization.hxx"
#include "../common/sf4e__RollbackDiagnostics.hxx"
#include "sf4e__NetplayFacade.hxx"
#include "sf4e__RuntimeSession.hxx"

namespace ember {

void Tick(GameTick& game) {
    namespace diag = sf4e::diag;
    namespace NetplayFacade = sf4e::NetplayFacade;
    auto& netplay = RuntimeSession::netplay;
    auto& server = RuntimeSession::server;
    {
        diag::ScopedTimer runtimeTimer(diag::OP_RUNTIME_TICK);
        NetplayFacade::TickRuntime();
    }

    if (netplay) {
        netplay->client.PrepareForCallbacks();
    }
    if (server) {
        server->PrepareForCallbacks();
    }

    bool netplayStepFailed = false;
    if (netplay) {
        diag::ScopedTimer _t(diag::OP_SESSION_CLIENT_STEP);
        int stepResult = netplay->client.Step();
        // A refused join is reported when the room fails, not as a lost room.
        if (stepResult < 0 && !netplay->client.JoinRejection()) {
            netplayStepFailed = true;
        }
    }

    bool serverStepFailed = false;
    if (server) {
        diag::ScopedTimer _t(diag::OP_SESSION_SERVER_STEP);
        if (server->Step() < 0) {
            serverStepFailed = true;
        }
    }

    // Reported every tick. On the loss edge an active healthy GGPO fight
    // degrades instead of being killed; outside that case the loss falls
    // back to full failure (Phase 7). The reason is read only on that edge, so
    // a healthy tick does not look it up.
    const bool sessionHealthy = !netplayStepFailed && !serverStepFailed;
    NetplayFacade::ObserveControlPlane(
        NetplayFacade::ControlPlaneCause::SessionClient,
        sessionHealthy,
        sessionHealthy ? "" : sf4e::loc::T(netplayStepFailed ? "runtime.room_link_lost" : "runtime.session_server_error")
    );
    // The room's updates of this tick are in: the Training call is reconciled
    // with them before the next battle update can leave for it.
    NetplayFacade::ReconcileTrainingCall();

    {
        diag::ScopedTimer _t(diag::OP_FACADE_TICK_FRAME);
        NetplayFacade::TickFrame();
    }

    game.PollBattle();
    NetplayFacade::PollSpectatorExit();
    game.UpdateGame();
    // Everything this tick changed is settled: hand the overlay its next frame.
    NetplayFacade::PublishPresentationFrame();
}

} // namespace ember
