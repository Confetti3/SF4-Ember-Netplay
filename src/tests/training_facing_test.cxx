// Which way the Training fighters face (TrainingSession.hxx: Session::SetPositions):
// a slot, a loaded file and a typed reply are held for a fighter facing right
// and played for the way the fighter faces on each frame, a recording is held
// that way whichever side it was made on, and Mirror turns a slot by hand.
#include "../training/FighterPlacement.hxx"
#include "../training/MoveInputs.hxx"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace sf4e::training;
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
std::vector<Input> Synth(const std::vector<std::string>& steps, bool facingRight) {
    sf4e::combo::Compiled compiled; std::string error;
    Require(sf4e::combo::Synthesize(steps, facingRight, compiled, error), "A move did not synthesize");
    return compiled.frames;
}
int main() {
    try {
        Require(MirrorDirections(LeftBit | 2 | 0x10) == (RightBit | 2 | 0x10) && MirrorDirections(LeftBit | RightBit) == (LeftBit | RightBit) &&
            MirrorDirections(1 | 0x800) == (1 | 0x800), "Mirroring changed more than Left and Right");
        // The two facings of a typed move differ by Left and Right alone,
        // so one held facing and the swap stand for both.
        for (const char* line : {"623HP", "41236LP", "[4]6HP", "2MK", "236P", "63214HK", "44", "MP+MK", "j.HK", "8"}) {
            const auto right = Synth({line}, true), left = Synth({line}, false);
            Require(right.size() == left.size(), "The two facings of a typed move differ in length");
            for (std::size_t frame = 0; frame < right.size(); ++frame) {
                const auto turned = Mirror(right[frame]);
                Require(turned.raw == left[frame].raw && turned.mapped == left[frame].mapped && right[frame].wait == left[frame].wait &&
                    right[frame].offset == left[frame].offset, "The two facings of a typed move differ by more than Left and Right");
            }
        }
        const auto hcf = Synth({"41236LP"}, true), hcfLeft = Synth({"41236LP"}, false);
        const unsigned L = LeftBit, R = RightBit, D = 2, LP = 0x10;
        // Plays a whole reply on the dummy, which turns to face right before frame turnAt (-1 never).
        const auto play = [&](Session& session, int turnAt) {
            std::vector<unsigned> played;
            for (int frame = 0; session.GetView().mode == Mode::Playback && frame < 100; ++frame) {
                if (frame == turnAt) session.SetPositions(100, 0);
                const auto out = session.Prepare(Frame{});
                played.push_back(out[1].raw); session.Commit(out);
            }
            return played;
        };
        // Neither fighter has moved: the dummy starts on the right, facing left.
        Session typed; typed.Enter(); typed.SetReady(true);
        Require(!typed.FacingRight(1) && typed.FacingRight(0), "The fighters did not start facing each other");
        Require(typed.Reply(hcf, ReplySchedule{}), "The typed reply did not start");
        auto played = play(typed, -1);
        Require(played.size() == hcfLeft.size(), "The typed reply did not play whole");
        // Turned once, for the dummy facing left: 41236P comes out as it
        // is typed there, not turned back, so it never ends in 214.
        for (std::size_t frame = 0; frame < played.size(); ++frame)
            Require(played[frame] == hcfLeft[frame].raw, "A typed reply facing left was not its left-facing input, or was turned twice");
        Require(played[0] == R && played[12] == L, "41236 facing left did not start back (Right) and end forward (Left)");
        // The sides swap in the middle of the reply (a cross-up on wake-up):
        // each frame plays for the facing it is played in.
        Session swapped; swapped.Enter(); swapped.SetReady(true); swapped.SetPositions(0, 100);
        Require(swapped.Reply(hcf, ReplySchedule{}), "The typed reply did not start again");
        played = play(swapped, 6);
        Require(played.size() == hcf.size(), "The turned reply did not play whole");
        for (std::size_t frame = 0; frame < played.size(); ++frame)
            Require(played[frame] == (frame < 6 ? hcfLeft[frame].raw : hcf[frame].raw), "A typed reply kept the facing it was decided with");
        const auto press = std::find_if(hcf.begin(), hcf.end(), [&](const Input& input) { return (input.raw & LP) != 0; }) - hcf.begin();
        Require(played[press] == (R | LP), "The press after the dummy turned was not forward for the new side");
        // Level, each fighter keeps the way it faced.
        swapped.SetPositions(50, 50);
        Require(swapped.FacingRight(1) && !swapped.FacingRight(0), "Fighters level with each other took a default facing");
        // A recording on the left, facing right, is held as pressed; played
        // on the right it comes out with Left and Right swapped, and on the
        // left as it was recorded. Player 1's pad plays the dummy.
        Session recorded; recorded.Enter(); recorded.SetReady(true);
        const auto send = [&](Action action) { Command command; command.action = action; command.generation = recorded.GetView().generation; return recorded.Apply(command); };
        const std::vector<unsigned> dragon{R, D, D | R, D | R | LP};
        recorded.SetPositions(100, 0);
        Require(send(Action::Record), "The recording did not start");
        for (const unsigned bits : dragon) { Frame pad; pad[0] = {bits, bits, 0, 0}; recorded.Commit(recorded.Prepare(pad)); }
        Require(send(Action::Stop) && recorded.Slot(0).size() == dragon.size(), "The recording was not kept");
        for (std::size_t frame = 0; frame < dragon.size(); ++frame)
            Require(recorded.Slot(0)[frame].raw == dragon[frame] && recorded.Slot(0)[frame].mapped == dragon[frame], "A recording facing right was not held as pressed");
        Command once; once.action = Action::Loop; once.generation = recorded.GetView().generation; once.loop = false;
        Require(recorded.Apply(once), "Looping was not turned off");
        const auto playSlot = [&](float x0, float x1) {
            recorded.SetPositions(x0, x1);
            std::vector<unsigned> out;
            Require(send(Action::Play), "The playback did not start");
            while (recorded.GetView().mode == Mode::Playback && out.size() < 20) {
                const auto frame = recorded.Prepare(Frame{}); out.push_back(frame[1].raw);
                Require(frame[1].mapped == frame[1].raw, "A played frame's two words disagree");
                recorded.Commit(frame);
            }
            return out;
        };
        Require(playSlot(0, 100) == std::vector<unsigned>{L, D, D | L, D | L | LP}, "A recording made on the left did not swap Left and Right on the right");
        Require(recorded.GetView().history[1].front().buttons == (D | L | LP), "The input history did not show the direction played");
        Require(playSlot(100, 0) == dragon, "A recording played on the side it was made on changed");
        // Made on the right, facing left, with the sides swapping halfway:
        // forward is Left for two frames, then Right. The slot holds
        // forward throughout, and the history what the pad pressed.
        Command select; select.action = Action::Select; select.generation = recorded.GetView().generation; select.slot = 1;
        Require(recorded.Apply(select), "The second slot was not selected");
        recorded.SetPositions(0, 100);
        Require(send(Action::Record), "The second recording did not start");
        for (const unsigned bits : {L, L | LP, R, R | LP}) {
            Frame pad; pad[0] = {bits, bits, 0, 0}; recorded.Commit(recorded.Prepare(pad));
            if (bits == (L | LP)) recorded.SetPositions(100, 0);
        }
        Require(recorded.GetView().history[1].front().buttons == (R | LP) && recorded.GetView().history[1][2].buttons == (L | LP),
            "The input history did not show the pad's own directions");
        Require(send(Action::Stop) && recorded.Slot(1).size() == 4, "The second recording was not kept");
        for (std::size_t frame = 0; frame < 4; ++frame)
            Require((recorded.Slot(1)[frame].raw & (L | R)) == R, "A side change in a recording was not held as forward throughout");
        Require(playSlot(0, 100) == std::vector<unsigned>{L, L | LP, L, L | LP}, "A recording with a side change did not play forward on the right");
        // A reply from a slot plays for the way the dummy faces too.
        Require(recorded.Reply(1, ReplySchedule{}) && recorded.Prepare(Frame{})[1].raw == L, "A reply from a slot did not turn for the dummy facing left");
        recorded.StopReply();
        // Mirror swaps the selected slot's Left and Right, once per press,
        // and never while it plays or when it is empty.
        Require(send(Action::Mirror) && recorded.Slot(1)[0].raw == L && recorded.Slot(1)[1].raw == (L | LP) &&
            recorded.Slot(1)[1].mapped == (L | LP), "Mirror did not swap the slot's Left and Right");
        Require(send(Action::Mirror) && recorded.Slot(1)[1].raw == (R | LP), "Mirror twice did not give the slot back");
        Require(send(Action::Play) && !send(Action::Mirror) && recorded.Slot(1)[1].raw == (R | LP), "Mirror changed a slot while it played");
        Require(send(Action::Stop) && send(Action::Clear) && !send(Action::Mirror), "Mirror took an empty slot");
        // Loaded input is held as a slot is: on Player 1, facing right, it plays as loaded.
        Command load; load.action = Action::Load; load.generation = recorded.GetView().generation; load.side = 0; load.frames = {Input{R, R, 0, 0}};
        recorded.SetPositions(0, 100);
        Require(recorded.Apply(load) && send(Action::Play) && recorded.Prepare(Frame{})[0].raw == R, "Loaded input on Player 1 facing right was turned");
        recorded.SetPositions(100, 0);
        Require(recorded.Prepare(Frame{})[0].raw == L, "Loaded input on Player 1 did not turn with Player 1");
        // Loaded and mirrored slots play once or looped as Play's choice says, as a recording does.
        Require(send(Action::Stop), "The loaded playback did not stop");
        load.side = 1; load.frames = {Input{R, R, 0, 0}, Input{D, D, 0, 0}};
        Require(recorded.Apply(load), "The two-frame load was refused");
        // Frames played before the playback stops, at most six.
        const auto framesPlayed = [&](bool loop) {
            Command choice; choice.action = Action::Loop; choice.generation = recorded.GetView().generation; choice.loop = loop;
            Require(recorded.Apply(choice) && send(Action::Play), "The playback did not start");
            int frames = 0;
            for (; recorded.GetView().mode == Mode::Playback && frames < 6; ++frames) recorded.Commit(recorded.Prepare(Frame{}));
            send(Action::Stop);
            return frames;
        };
        Require(framesPlayed(false) == 2 && framesPlayed(true) == 6, "A loaded recording did not follow Play's once or looped");
        Require(send(Action::Mirror) && framesPlayed(false) == 2 && framesPlayed(true) == 6, "A mirrored recording did not follow Play's once or looped");
        // A Place as the runtime carries it out: the session only accepts the
        // command, and MoveFighters moves both fighters, or neither, and turns
        // the input of the update it comes in, the frame a recording holds
        // and the frame a playback plays.
        Session placed; placed.Enter(); placed.SetReady(true);
        const auto order = [&](Action action) { Command command; command.action = action; command.generation = placed.GetView().generation; return placed.Apply(command); };
        // Each fighter's root position as the game holds it, x first.
        float first[4] = {0, 0, 0, 0}, second[4] = {100, 0, 0, 0};
        const float swap[2] = {100, 0};
        const FighterRoots both{{first, second}};
        placed.SetPositions(first[0], second[0]);
        Command place; place.action = Action::Place; place.generation = placed.GetView().generation; place.place[0] = 100; place.place[1] = 0;
        Require(placed.Apply(place) && !placed.FacingRight(1) && placed.GetView().x[0] == 0, "Accepting a Place moved the session before the fighters moved");
        // The game has no second fighter: neither moves and the facing stays.
        for (const FighterRoots missing : {FighterRoots{{first, nullptr}}, FighterRoots{{nullptr, second}}, FighterRoots{}}) {
            Require(!MoveFighters(placed, missing, swap) && first[0] == 0 && second[0] == 100 && !placed.FacingRight(1) && placed.GetView().x[0] == 0,
                "A Place that failed moved a fighter or turned the session");
        }
        Require(order(Action::Record), "The recording did not start");
        { Frame pad; pad[0] = {L, L, 0, 0}; placed.Commit(placed.Prepare(pad)); }
        Require(MoveFighters(placed, both, swap) && first[0] == 100 && second[0] == 0 && placed.FacingRight(1) && placed.GetView().x[0] == 100,
            "A Place that swapped the fighters did not move and turn them");
        { Frame pad; pad[0] = {R, R, 0, 0}; placed.Commit(placed.Prepare(pad)); }
        Require(order(Action::Stop) && placed.Slot(0).size() == 2 && placed.Slot(0)[0].raw == R && placed.Slot(0)[1].raw == R,
            "A recording after a Place that swapped the fighters was held for the old facing");
        const float back[2] = {0, 100};
        Require(MoveFighters(placed, both, back) && order(Action::Play) && placed.Prepare(Frame{})[1].raw == L, "The playback did not play for the dummy facing left");
        Require(MoveFighters(placed, both, swap) && placed.Prepare(Frame{})[1].raw == R, "A playback after a Place that swapped the fighters played for the old facing");
        std::puts("Training facing checks passed.");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
