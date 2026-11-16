#pragma once
#include "DummyReply.hxx"
#include "TrainingCommands.hxx"
#include "TrainingSession.hxx"
#include <cstdint>
#include <deque>
#include <mutex>
#include <random>
#include <utility>
#include <vector>

// What a game's Training lab shares with every other game's, apart from the
// model (TrainingSession.hxx, DummyReply.hxx): the one place the game thread
// and the overlay meet, and the dummy's reply as one battle follows it. Each
// game's runtime keeps one of each and does its own reading and writing of
// the game around them (SF4: TrainingRuntime.cxx; SFV Ember: its
// SfvTrainingLab). Nothing here knows a game.
namespace sf4e { namespace training {
// The lab as the overlay sees it and the commands it sends: the view the
// game thread published after its last update, the queue the overlay fills
// and the room's call for this battle. Any thread; one lock.
class LabChannel {
public:
    View Read() const { std::lock_guard<std::mutex> lock(mutex_); return published_; }
    // Where the fighters stand and the battle they stand in, without the rest
    // of the view: what a pad press keeps for the save it may become.
    Place ReadPlace() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Place place; place.generation = published_.generation; place.x[0] = published_.x[0]; place.x[1] = published_.x[1];
        return place;
    }
    // Whether the pad's chord starts the recording rather than opening the
    // controls, as the mode was last published.
    bool ChordStartsRecording() const { std::lock_guard<std::mutex> lock(mutex_); return training::ChordStartsRecording(published_.mode); }
    // False when refused: no battle, another battle's, or a full queue.
    bool Submit(const Command& command) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!published_.available || command.generation != published_.generation) return false;
        return commands_.Push(command);
    }
    void SetCall(const CallControl& call) { std::lock_guard<std::mutex> lock(mutex_); call_ = call; }
    CallControl Call() const { std::lock_guard<std::mutex> lock(mutex_); return call_; }
    // Game thread: this update's commands, and those replaced since the last.
    void Take(std::deque<Command>& taken, std::vector<std::uint64_t>& replaced) { std::lock_guard<std::mutex> lock(mutex_); commands_.Take(taken, replaced); }
    // Game thread: the view after an update. edit runs under the lock with
    // the view as last published, so a field only one path sets (a match
    // meter's) can be kept.
    template<class Edit> void Publish(Edit&& edit) { std::lock_guard<std::mutex> lock(mutex_); edit(published_); }
    // Game thread: the battle closed; its commands go unanswered and the view is the session's at rest.
    void Close(const View& view) { std::lock_guard<std::mutex> lock(mutex_); commands_.Clear(); published_ = view; }
private:
    mutable std::mutex mutex_;
    View published_;
    CommandQueue commands_;
    CallControl call_;
};

// The dummy's reply over a game's Training battles: the player's plan, what
// the stun watch saw of the dummy, the reply waiting or playing, its trace
// for the log and the random numbers the chance and the slots draw. The
// plan outlives a battle; the rest goes with each. Game thread.
class ReplyLab {
public:
    DummyPlan plan;
    explicit ReplyLab(unsigned seed = std::random_device{}()) : random_(seed) {}
    // A battle begins: the session's pickers and the reply's draw are seeded anew.
    void Seed(Session& session) { session.Seed(random_()); replies_.Seed(random_()); }
    // Once per frame the game simulated, with the dummy as the game left it:
    // attacker, the other fighter's action; scripted, frames to its free frame
    // by its own count, -1 when the game does not tell.
    DummySeen Watch(const FighterSample& dummy, int attacker, int scripted) { return watch_.Observe(dummy, attacker, scripted); }
    // Whether the reply playing is due to let go of its leading hold, for the
    // session's waits (Session::Observe's replyDue), from this frame's watch.
    static bool Due(const Session& session, const DummySeen& seen) {
        return session.Replying() && ReplyFirstMoveDue(session.Schedule(), seen);
    }
    // Once the session has committed the frame: the reply decided, begun or dropped.
    ReplyStep Decide(const DummySeen& seen, Session& session) { return replies_.Observe(plan, seen, session, random_()); }
    // Follows each reply to where its button met the dummy (ReplyTrace).
    bool Trace(Session& session, bool pressed, const DummySeen& seen, bool moving, ReplyTiming& timing) {
        return trace_.Observe(session.Replies(), session.Replying(), pressed, seen, moving, session.Schedule().timing, timing);
    }
    bool Pending() const { return replies_.Pending() != nullptr; }
    // A coin for the game's own choices made at random (a stance).
    unsigned Draw() { return random_(); }
    // The frames stopped being consecutive (a gap, a reset): the stun watch
    // and a reply that waited on it go.
    void Forget(Session& session) { ForgetStun(watch_, session, replies_); trace_.Reset(); }
    // The battle closed.
    void Close() { watch_.Reset(); replies_.Reset(); trace_.Reset(); }
private:
    DummyWatch watch_;
    ReplyDecision replies_;
    ReplyTrace trace_;
    std::minstd_rand random_;
};
} }
