#include "../common/RollbackHud.hxx"
#include "../common/MatchTelemetry.hxx"
#include <cstdlib>
#include <iostream>
#include "test_support.hxx"
int main() {
    sf4e::MatchTelemetry telemetry;
    CHECK(telemetry.Ping(0)==-1 && telemetry.appliedDelay==-1);
    CHECK(telemetry.PollDue(0));CHECK(!telemetry.PollDue(249));CHECK(telemetry.PollDue(250));
    telemetry.Sample(250,68);CHECK(telemetry.Ping(2250)==68);CHECK(telemetry.Ping(2251)==-1);
    telemetry.Sample(2300,-1);CHECK(telemetry.Ping(2300)==-1);
    // H-010: GGPO's 0 before the first round trip is not a ping.
    telemetry.Sample(2400,0);CHECK(telemetry.Ping(2400)==-1);
    telemetry.Sample(2500,55);CHECK(telemetry.Ping(2500)==55);
    telemetry.AppliedDelay(3,true);CHECK(telemetry.appliedDelay==3);
    telemetry.AppliedDelay(5,false);CHECK(telemetry.appliedDelay==-1);
    telemetry.Reset(true);CHECK(telemetry.spectator&&!telemetry.PollDue(5000));
    telemetry.Sample(5000,40);CHECK(telemetry.Ping(5000)==-1);
    telemetry.Reset();CHECK(!telemetry.spectator&&telemetry.appliedDelay==-1&&telemetry.Ping(5000)==-1&&telemetry.PollDue(5000));
    sf4e::RollbackHud hud;
    CHECK(hud.Recent(0) == 0);
    hud.Begin(10);
    for (int i = 0; i < 5; ++i) hud.Replayed(10);
    hud.Begin(500); hud.Replayed(500); hud.Replayed(500);
    CHECK(hud.Recent(1009) == 5);
    CHECK(hud.Recent(1010) == 2);
    CHECK(hud.Recent(1500) == 0);
    hud.Begin(1600); CHECK(hud.Recent(1600) == 0);
    hud.Replayed(1600); hud.Reset(); CHECK(hud.Recent(1600) == 0);
    std::cout << "Rollback depth, expiry, empty loads and session reset passed\n";
}
