#pragma once
#include "RecordingTypes.hxx"
#include <cstdint>
#include <string>
#include <vector>

// Combo check: Player 1's own combo, recorded once, played against each
// fighter of the cast as the dummy, standing and crouching, and how many of
// its hits landed on each. Only the logic; the runtime feeds it what the
// fight shows and carries out its steps with Training's own commands.
namespace sf4e { namespace training { namespace combo_check {

// Frames without a press, after the first one, that end a recording.
constexpr int IdleStop = 60;

// The hits on the dummy in one combo: a hit counts while the dummy is still
// hit or blocking from the one before, and a combo ends when it is free.
// best: the longest combo since the last Reset.
class HitTally {
public:
    void Reset() { combo_ = best_ = 0; }
    void Observe(bool hit, bool dummyFree) {
        if (dummyFree && !hit) { combo_ = 0; return; }
        if (hit && ++combo_ > best_) best_ = combo_;
    }
    int Best() const { return best_; }
private:
    int combo_ = 0, best_ = 0;
};

// Player 1's combo as it was done: its frames for a fighter facing right
// (as a slot holds them), where both fighters stood on the frame before its
// first press, and the hits it landed then.
struct Combo { std::vector<Input> frames; float x[2] = {0, 0}; int hits = 0; int fighter = -1; };  // fighter: Player 1's, -1 unknown

// Records Player 1 from the first press until IdleStop frames without one,
// or MaxFrames; the frames after the last press are dropped. Each frame is
// kept as pressed, without a playback's waits, so it replays frame for frame.
// Its hits are the tally's best, so the tally starts again with it.
class Recorder {
public:
    void Begin() { *this = Recorder{}; armed_ = true; }
    bool Armed() const { return armed_; }
    // One simulated frame: Player 1's input for its facing, and where both stood before it.
    // True on the frame the recording ends.
    bool Feed(Input input, float x0, float x1, const HitTally& tally) {
        if (!armed_) return false;
        const bool pressed = (input.raw & FightButtons) != 0;
        if (combo_.frames.empty()) {
            if (!pressed) { combo_.x[0] = x0; combo_.x[1] = x1; return false; }
        }
        combo_.frames.push_back({input.mapped, input.raw, 0, 0});
        if (pressed) { idle_ = 0; last_ = combo_.frames.size(); } else ++idle_;
        if (idle_ < IdleStop && combo_.frames.size() < std::size_t(MaxFrames)) return false;
        return End(tally);
    }
    // Ends the recording now, as IdleStop does (an interruption ends it so):
    // true when it holds a combo, false when nothing was pressed yet.
    bool End(const HitTally& tally) {
        if (!armed_) return false;
        armed_ = false;
        if (combo_.frames.empty()) return false;
        combo_.frames.resize(last_);
        combo_.hits = tally.Best();
        return true;
    }
    const Combo& Result() const { return combo_; }
private:
    Combo combo_;
    bool armed_ = false;
    int idle_ = 0;
    std::size_t last_ = 0;
};

enum class Start { Recorded, Middle, Corner };
// Where the stage's walls stop a fighter: the scroll limit (5, at 0xaa720c)
// plus the screen's half width (2.5, at 0xaa7210), each side of x 0, as
// clampToStageWalls (0x5436e0) holds them.
constexpr float StageWall = 7.5f;
struct Settings { std::vector<int> fighters; bool standing = true, crouching = true; Start start = Start::Recorded; bool fast = false; };

// One run: a fighter as the dummy, standing or crouching, and what came of it.
struct Run { int fighter = 0; bool crouching = false; bool done = false; int hits = 0; };
// Each fighter once, so it loads once: standing, then crouching.
inline std::vector<Run> PlanRuns(const Settings& settings) {
    std::vector<Run> runs;
    for (const int fighter : settings.fighters) {
        if (settings.standing) runs.push_back({fighter, false});
        if (settings.crouching) runs.push_back({fighter, true});
    }
    return runs;
}

// What the runtime reads of the fight for Combo check, published each frame.
// dummy: the dummy's fighter, -1 unknown. best: HitTally::Best. free: both
// fighters free. recording: a Recorder is armed. comboId moves on with each
// recording that ends (the combo itself: TrainingRuntime LastCombo).
struct FightView {
    int dummy = -1, p1 = -1, best = 0;
    bool free = false, recording = false;
    std::uint64_t comboId = 0;
    // The last combo's hits and frames as recorded; a check runs, and its runs so far.
    int hits = 0, frames = 0;
    bool running = false;
    std::vector<Run> runs;
};

// Where both fighters start a run: as recorded, or with the same gap, each on
// the side it had, around the stage's middle (x 0) or with the dummy on the
// wall behind it.
inline void StartAt(const Combo& combo, Start start, float (&x)[2]) {
    const float gap = combo.x[1] - combo.x[0];
    x[0] = combo.x[0]; x[1] = combo.x[1];
    if (start == Start::Middle) { x[0] = -gap / 2; x[1] = gap / 2; }
    if (start == Start::Corner) { x[1] = gap < 0 ? -StageWall : StageWall; x[0] = x[1] - gap; }
}

// What the runner reads of the fight on each poll.
struct Seen {
    std::uint64_t generation = 0;
    bool ready = false;
    // The dummy's fighter, -1 unknown.
    int dummy = -1;
    // A playback is on, and both fighters are free.
    bool playing = false, free = false;
    int best = 0;
};
// What the runtime does next: load a fighter as the dummy, or start a run
// (place both, set the stance, play the combo on Player 1).
enum class Do { Wait, Load, Play, Finished };
struct Step { Do what = Do::Wait; int fighter = -1; bool crouching = false; };

// Polls of a free, ready fight before a run starts, and after it ends.
constexpr int SettlePolls = 20;
// Polls a fight may stay ready after a load was asked for before it is asked for again.
constexpr int LoadPolls = 120;

// A check from Begin to its end, the one place that says when it ends: its
// last run done, or Stop (the player's Stop, an interruption, a fighter that
// cannot be loaded), or the battle closing (BattleClosed) for anything but
// the load the check itself asked for. The runs done stay to be read.
class Runner {
public:
    void Begin(const Settings& settings, Combo combo) { *this = Runner{}; runs_ = PlanRuns(settings); combo_ = std::move(combo); start_ = settings.start; fast_ = settings.fast; }
    bool Running() const { return next_ < runs_.size(); }
    // The run under way, done or not, counts for nothing.
    void Stop() { runs_.resize(next_); reloading_ = false; }
    // The battle closes: the check goes on through the battle closing for the
    // load it asked for, once; any other close ends it. False when it ended.
    bool BattleClosed() {
        if (reloading_) { reloading_ = false; return true; }
        Stop(); return false;
    }
    // Training plays several updates a frame while the check runs (Settings::fast).
    bool Fast() const { return fast_ && Running(); }
    const std::vector<Run>& Runs() const { return runs_; }
    const Combo& GetCombo() const { return combo_; }
    Start GetStart() const { return start_; }
    Step Next(const Seen& seen) {
        if (!Running()) return {Do::Finished};
        Run& run = runs_[next_];
        if (seen.dummy != run.fighter) {
            // A load under way leaves the fight; only one that never did is asked for again.
            if (state_ == State::Loading && (!seen.ready || (seen.generation == loadedFrom_ && ++settle_ < LoadPolls))) return {};
            state_ = State::Loading; loadedFrom_ = seen.generation; settle_ = 0; reloading_ = true;
            return {Do::Load, run.fighter};
        }
        if (!seen.ready) { settle_ = 0; return {}; }
        switch (state_) {
        case State::Loading: case State::Settling:
            state_ = State::Settling;
            if (!seen.free || ++settle_ < SettlePolls) return {};
            // The run plays from here: Training starts it in this update
            // (NotStarted when it does not), and a clip of one frame may be
            // over before the next poll.
            state_ = State::Playing; settle_ = 0;
            return {Do::Play, run.fighter, run.crouching};
        case State::Playing:
            if (seen.playing || !seen.free) { settle_ = 0; return {}; }
            if (++settle_ < SettlePolls) return {};
            run.hits = seen.best; run.done = true;
            ++next_; state_ = State::Settling; settle_ = 0;
            return Next(seen);
        }
        return {};
    }
    // Training refused the run Next asked to play: it settles and is asked for again.
    void NotStarted() { state_ = State::Settling; settle_ = 0; }
private:
    enum class State { Loading, Settling, Playing };
    std::vector<Run> runs_;
    Combo combo_;
    Start start_ = Start::Recorded;
    bool fast_ = false;
    std::size_t next_ = 0;
    State state_ = State::Settling;
    std::uint64_t loadedFrom_ = 0;
    // A load was asked for and the battle has not yet closed for it.
    bool reloading_ = false;
    int settle_ = 0;
};

// A run landed every hit the combo landed when it was recorded.
inline bool Landed(const Run& run, int hits) { return run.done && hits > 0 && run.hits >= hits; }

} } }
