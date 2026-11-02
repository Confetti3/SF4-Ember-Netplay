// Combo check's logic (training/ComboCheck.hxx): the hit tally, Player 1's
// recording, the runs and the runner's steps against a made-up fight.
// ComboPlay in the session: training_session_test.cxx.
#include "../training/TrainingSession.hxx"
#include <cstdio>
#include <stdexcept>
using namespace sf4e::training;
using namespace sf4e::training::combo_check;
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
Input Press(unsigned raw) { return {raw, raw, 0, 0}; }

int main() {
    try {
        {
            HitTally tally;
            tally.Observe(true, false); tally.Observe(false, false); tally.Observe(true, false);
            Require(tally.Best() == 2, "Two hits before the dummy was free are one combo of 2");
            tally.Observe(false, true); tally.Observe(true, false);
            Require(tally.Best() == 2, "A hit after the dummy was free starts a new combo");
            tally.Observe(true, true);
            Require(tally.Best() == 2, "A hit on the frame the dummy shows free still counts in its combo");
            tally.Observe(true, false);
            Require(tally.Best() == 3, "and the next hit makes it 3");
            tally.Reset();
            Require(tally.Best() == 0, "Reset forgets the best combo");
        }
        {
            HitTally tally; Recorder recorder; recorder.Begin();
            Require(!recorder.Feed(Press(0), -1, 1, tally) && !recorder.Feed(Press(0), -1.5f, 1, tally), "Frames before the first press are not recorded");
            Require(!recorder.Feed({0x10, 0x10, WaitHit, 3}, -1, 1, tally), "The first press starts the recording");
            tally.Observe(true, false);
            recorder.Feed(Press(0), -1, 1, tally);
            recorder.Feed(Press(0x20), -1, 1, tally);
            tally.Observe(true, false);
            bool ended = false; int idle = 0;
            while (!ended) { ended = recorder.Feed(Press(0), 0, 0, tally); ++idle; }
            const Combo& combo = recorder.Result();
            Require(idle == IdleStop, "The recording ends IdleStop frames after the last press");
            Require(combo.frames.size() == 3 && combo.frames[0].raw == 0x10 && combo.frames[2].raw == 0x20, "The frames after the last press are dropped");
            Require(combo.frames[0].wait == 0 && combo.frames[0].offset == 0, "A frame is kept as pressed, without a playback's wait");
            Require(combo.x[0] == -1.5f && combo.x[1] == 1, "The start is where both stood before the first press");
            Require(combo.hits == 2, "The combo's hits are those it landed");
            Require(!recorder.Armed(), "An ended recording is not armed");
        }
        {
            Settings settings; settings.fighters = {0, 7};
            const auto runs = PlanRuns(settings);
            Require(runs.size() == 4 && runs[0].fighter == 0 && !runs[0].crouching && runs[1].fighter == 0 && runs[1].crouching && runs[2].fighter == 7,
                "Each fighter loads once: standing, then crouching");
            settings.standing = false;
            Require(PlanRuns(settings).size() == 2, "Crouching only is one run a fighter");
            Combo combo; combo.x[0] = 2; combo.x[1] = 3;
            float x[2];
            StartAt(combo, Start::Middle, x);
            Require(x[0] == -0.5f && x[1] == 0.5f, "The middle keeps the gap around x 0");
            StartAt(combo, Start::Corner, x);
            Require(x[1] == StageWall && x[0] == StageWall - 1, "The corner puts the dummy on the wall behind it");
            combo.x[0] = 3; combo.x[1] = 2;
            StartAt(combo, Start::Corner, x);
            Require(x[1] == -StageWall && x[0] == 1 - StageWall, "and on the other wall when it stands on the left");
            combo.x[0] = 2; combo.x[1] = 3;
            StartAt(combo, Start::Recorded, x);
            Require(x[0] == 2 && x[1] == 3, "Recorded keeps where they stood");
        }
        {
            Settings settings; settings.fighters = {5}; settings.crouching = false;
            Combo combo; combo.hits = 3;
            Runner runner; runner.Begin(settings, combo);
            Seen seen; seen.generation = 1; seen.ready = true; seen.dummy = 0; seen.free = true;
            Step step = runner.Next(seen);
            Require(step.what == Do::Load && step.fighter == 5, "Another dummy is loaded first");
            Require(runner.Next(seen).what == Do::Wait, "The load is asked for once");
            for (int i = 2; i < LoadPolls; ++i) runner.Next(seen);
            Require(runner.Next(seen).what == Do::Load, "A load that never left the battle is asked for again");
            seen.ready = false;
            for (int i = 0; i < 2 * LoadPolls; ++i) Require(runner.Next(seen).what == Do::Wait, "A load that left the fight is waited for");
            seen.ready = true; seen.generation = 2; seen.dummy = 5;
            for (int i = 1; i < SettlePolls; ++i) Require(runner.Next(seen).what == Do::Wait, "The fight settles before the run");
            step = runner.Next(seen);
            Require(step.what == Do::Play && step.fighter == 5 && !step.crouching, "The run starts once the fight has settled");
            Require(runner.Next(seen).what == Do::Wait, "Starting waits for the playback");
            for (int i = 2; i < LoadPolls + SettlePolls; ++i) Require(runner.Next(seen).what == Do::Wait, "A run that never plays settles again");
            Require(runner.Next(seen).what == Do::Play, "and is started again");
            seen.playing = true; seen.free = false; seen.best = 2;
            runner.Next(seen);
            seen.playing = false; seen.free = true;
            for (int i = 1; i < SettlePolls; ++i) Require(runner.Next(seen).what == Do::Wait, "The run's end settles");
            Require(runner.Next(seen).what == Do::Finished && !runner.Running(), "The last run done, the check is finished");
            Require(runner.Runs()[0].done && runner.Runs()[0].hits == 2 && !Landed(runner.Runs()[0], combo), "2 of 3 hits is a miss");
        }
        {
            Settings settings; settings.fighters = {1, 2, 3};
            Runner runner; runner.Begin(settings, Combo{});
            Seen seen; seen.dummy = 1;
            runner.Next(seen); runner.Stop();
            Require(!runner.Running() && runner.Runs().empty(), "Stop keeps only the runs done");
        }
        std::puts("Combo check tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
