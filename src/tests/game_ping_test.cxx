#include "../common/GamePing.hxx"
#include "../common/MatchTelemetry.hxx"
#include <iostream>
#include "test_support.hxx"
using sf4e::GamePing;
int main() {
    // A game that reported nothing has no ping to speak of.
    GamePing ping;
    GamePing::Summary summary = ping.Summarize();
    CHECK(summary.samples == 0 && summary.medianMs == 0 && summary.p95Ms == 0);

    // The median and p95 sit at sorted position ceil((samples - 1) * percent / 100),
    // as the connection check's do (recovery.rs: summarize_probe), so two
    // pings give the upper one for the median, as probe_delay_median_uses_the_displayed_percentile_index.
    ping.Add(100);
    summary = ping.Summarize();
    CHECK(summary.samples == 1 && summary.medianMs == 100 && summary.p95Ms == 100);
    ping.Add(171);
    summary = ping.Summarize();
    CHECK(summary.samples == 2 && summary.medianMs == 171 && summary.p95Ms == 171);

    // 100 pings of 1 to 100 ms: position 50 is 51 ms and position 95 is 96 ms.
    GamePing ramp;
    for (int ms = 1; ms <= 100; ++ms) ramp.Add(ms);
    summary = ramp.Summarize();
    CHECK(summary.samples == 100 && summary.medianMs == 51 && summary.p95Ms == 96);

    // A spike in the tail moves the p95 but not the median.
    GamePing spiky;
    for (int i = 0; i < 90; ++i) spiky.Add(40);
    for (int i = 0; i < 10; ++i) spiky.Add(400);
    summary = spiky.Summarize();
    CHECK(summary.medianMs == 40 && summary.p95Ms == 400);

    // Pings outside what a delay band could tell apart are held to the ends.
    GamePing extreme;
    extreme.Add(-5);
    extreme.Add(100000);
    summary = extreme.Summarize();
    CHECK(summary.samples == 2 && summary.medianMs == GamePing::MaximumMs && summary.p95Ms == GamePing::MaximumMs);

    // MatchTelemetry keeps what each poll measured, apart from the polls that
    // had no ping, and starts again with the next session.
    sf4e::MatchTelemetry telemetry;
    telemetry.Sample(250, 60);
    telemetry.Sample(500, -1);
    telemetry.Sample(750, 0);
    telemetry.Sample(1000, 80);
    summary = telemetry.pings.Summarize();
    CHECK(summary.samples == 2 && summary.medianMs == 80 && summary.p95Ms == 80);
    telemetry.Reset();
    CHECK(telemetry.pings.Summarize().samples == 0);
    // A spectator is never polled, so it has no game ping to report.
    telemetry.Reset(true);
    CHECK(!telemetry.PollDue(0) && telemetry.pings.Summarize().samples == 0);
    std::cout << "Game ping summary passed\n";
}
