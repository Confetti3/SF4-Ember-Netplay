// The lab's shared parts (training/LabCore.hxx): the channel the overlay and
// the game thread meet in, and the dummy's reply as a battle follows it. A
// game's runtime keeps one of each; this drives them as a runtime does, with
// no game.
#include "../training/LabCore.hxx"
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace sf4e::training;
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
FighterSample Fighter(unsigned status, int action = 0, float frame = 0, float damage = 0) {
    FighterSample sample; sample.valid = true; sample.status = status; sample.action = action; sample.actionFrame = frame; sample.comboDamage = damage;
    return sample;
}
int main() {
    try {
        // The channel: no command before a battle is published, only the
        // published battle's after, and the view as the game thread left it.
        LabChannel channel;
        Require(!channel.Submit({Action::Play, 1}), "A command was taken before any battle");
        Session session; session.Enter(); session.SetReady(true);
        channel.Publish([&](View& published) { published = session.GetView(); published.x[0] = 10; published.x[1] = 50; });
        const auto generation = session.GetView().generation;
        Require(channel.Read().available && channel.Read().generation == generation, "The view was not the published one");
        Require(channel.ReadPlace().generation == generation && channel.ReadPlace().x[1] == 50, "The place was not the published one");
        Require(!channel.Submit({Action::Play, generation + 1}), "Another battle's command was taken");
        Require(channel.Submit({Action::AdvanceRecording, generation}), "This battle's command was refused");
        std::deque<Command> taken; std::vector<std::uint64_t> replaced;
        channel.Take(taken, replaced);
        Require(taken.size() == 1 && taken[0].action == Action::AdvanceRecording, "The command did not reach the game thread");
        Require(session.Apply(taken[0]) && session.GetView().mode == Mode::Standby, "Record did not put the session on standby");
        channel.Publish([&](View& published) { published = session.GetView(); });
        Require(channel.ChordStartsRecording(), "The chord did not start the recording on standby");
        CallControl call; call.call = 3; call.generation = generation; channel.SetCall(call);
        Require(channel.Call().call == 3, "The call was not kept");
        channel.Submit({Action::Play, generation});
        channel.Close(View{});
        channel.Take(taken, replaced);
        Require(taken.empty() && !channel.Read().available, "Closing kept the battle's commands or its view");

        // SetFacing turns what plays as SetPositions does: P1 facing left
        // plays a slot's Right as Left.
        Session facing; facing.Enter(); facing.SetReady(true);
        Command load; load.action = Action::Load; load.generation = facing.GetView().generation; load.side = 0;
        load.frames = {Input{RightBit, RightBit}};
        Require(facing.Apply(load), "The slot did not load");
        Require(facing.Apply({Action::Play, facing.GetView().generation}), "The slot did not play");
        facing.SetFacing(false);
        Require(facing.Prepare(Frame{})[0].raw == LeftBit, "Facing left did not turn Right into Left");
        facing.SetFacing(true);
        Require(facing.Prepare(Frame{})[0].raw == RightBit, "Facing right turned the slot");

        // The reply lab: a recording that is on replies after a block, timed
        // by the game's own count (scripted), from the frame it is due.
        ReplyLab lab(7);
        lab.plan.when = 2;
        Session dummy; dummy.Enter(); dummy.SetReady(true); lab.Seed(dummy);
        const auto battle = dummy.GetView().generation;
        Command reply; reply.action = Action::Load; reply.generation = battle; reply.side = 1;
        reply.frames = {Input{2, 2}, Input{2 | 0x10, 2 | 0x10}};
        Require(dummy.Apply(reply), "The reply's slot did not load");
        bool began = false; int beganAt = -1;
        // Blockstun with 6, 5, ... frames left by the game's count, then free.
        for (int frame = 0; frame < 10 && !began; ++frame) {
            const int left = 6 - frame;
            const auto sample = left > 0 ? Fighter(ActorStatus::DamageGuard, 40, static_cast<float>(frame)) : Fighter(ActorStatus::Stand);
            const DummySeen seen = lab.Watch(sample, 600, left > 0 ? left : -1);
            dummy.Observe(false, false, -1, ReplyLab::Due(dummy, seen));
            dummy.Commit(Frame{});
            const ReplyStep step = lab.Decide(seen, dummy);
            if (step.started) { began = true; beganAt = frame; }
        }
        // The reply's first frame presses Down, its attack button lands one
        // frame later, acted on DummyInputLag after it is sent: it begins
        // when the free frame is two frames (lead) plus the lag ahead.
        Require(began && dummy.Replying(), "The reply did not begin");
        Require(beganAt == 6 - (1 + DummyInputLag + 1), "The reply did not begin on the frame that meets the free frame");
        // A gap forgets it.
        lab.Forget(dummy);
        Require(!dummy.Replying() && !lab.Pending(), "A gap left the reply playing");
        std::puts("training lab core: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "training lab core: %s\n", error.what());
        return 1;
    }
}
