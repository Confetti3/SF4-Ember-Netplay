#include <filesystem>
#include "../training/CameraLayer.hxx"
#include <cstring>
#include <fstream>
#include <cstdio>
#include <stdexcept>
// The phone camera's maths: offsets over the game's camera, and the dolly track.
using namespace sf4e::training::camera;
void Require(bool pass, const char* why) { if (!pass) throw std::runtime_error(why); }
bool Near(float a, float b, float e = 1e-3f) { return std::fabs(a - b) <= e; }
// A camera as the game holds one, in plain arrays: the hook's layer is laid over it.
struct FakeCamera {
    float view[16] = {}, proj[16] = {}, eye[3] = {0, 1, -5}, target[3] = {0, 1, 0}, roll = 0, aspect = 16.f / 9, fov = 0.7854f, unitView[16] = {}, unitProj[16] = {};
    FakeCamera() { ViewMatrix(ReadPose(At()), view); std::memcpy(unitView, view, sizeof view); proj[0] = proj[5] = proj[10] = proj[15] = 1; std::memcpy(unitProj, proj, sizeof proj); }
    Fields At() { return {view, proj, eye, target, &roll, &aspect, &fov, unitView, unitProj}; }
    bool operator==(const FakeCamera& other) const { return std::memcmp(this, &other, sizeof *this) == 0; }
};
float Distance(const Pose& p) { const Vec3 d = p.eye - p.target; return std::sqrt(Dot(d, d)); }
int main() {
    try {
        const Pose game{{0, 1, -5}, {0, 1, 0}, 0, 0.7854f};
        Require(Apply(game, {}).eye.z == game.eye.z && Apply(game, {}).fov == game.fov, "No offsets must leave the game's camera alone");
        Offsets yaw; yaw.yaw = 1.5707963f;
        const Pose side = Apply(game, yaw);
        Require(Near(Distance(side), 5) && Near(side.eye.y, 1) && Near(std::fabs(side.eye.x), 5) && Near(side.eye.z, 0) && Near(side.target.x, 0), "A quarter turn must put the eye at the side, same distance and height");
        Offsets pitch; pitch.pitch = 0.5f;
        const Pose above = Apply(game, pitch);
        Require(above.eye.y > game.eye.y + 1 && Near(Distance(above), 5) && Near(above.target.y, 1), "Positive pitch must raise the eye on the same arm");
        Offsets over; over.pitch = 99;
        Require(Apply(game, over).eye.y < 1 + 5.f, "Pitch must stop short of the pole");
        Offsets zoom; zoom.zoom = 0.5f;
        Require(Near(Distance(Apply(game, zoom)), 2.5f), "Zoom must scale the distance");
        Offsets pan; pan.panX = 1; pan.panY = 0.5f;
        const Pose slid = Apply(game, pan);
        Require(Near(Distance(slid), 5) && Near(slid.target.y, 1.5f) && Near(std::fabs(slid.target.x), 1), "Pan must move eye and target together");
        Require(Near(slid.eye.x - slid.target.x, game.eye.x - game.target.x), "Pan must keep the viewing direction");
        Offsets fov; fov.fov = 9; fov.roll = 0.2f;
        Require(Apply(game, fov).fov <= MaxFov && Near(Apply(game, fov).roll, 0.2f), "FOV must be limited and roll added");
        Offsets bad; bad.yaw = NAN; bad.zoom = INFINITY;
        const Pose safe = Apply(game, bad);
        Require(std::isfinite(safe.eye.x) && Near(Distance(safe), 5), "Not-a-number offsets must be neutral");
        // The dolly.
        std::vector<Key> keys;
        Key a; a.frame = 60; a.ease = Ease::Linear; Key b; b.frame = 20; b.at.yaw = 1; b.ease = Ease::Hold; b.at.zoom = 2;
        keys = {a, b, a};  // out of order, with a repeated frame
        Tidy(keys);
        Require(keys.size() == 2 && keys[0].frame == 20 && keys[1].frame == 60, "Keys must be sorted, one per frame");
        Require(Sample(keys, 0).yaw == 1 && Sample(keys, 99).yaw == 0, "Before the first and after the last key hold their values");
        Require(Sample(keys, 40).yaw == 1, "Hold must keep the earlier key until the next");
        keys[0].ease = Ease::Linear;
        Require(Near(Sample(keys, 40).yaw, 0.5f) && Near(Sample(keys, 40).zoom, 1.5f), "Linear must be half way at the middle");
        keys[0].ease = Ease::Smooth;
        Require(Near(Sample(keys, 30).yaw, 0.84375f), "Smooth must ease");
        Require(Sample({}, 5).Identity(), "No track is no offset");
        // The phone's orders.
        Control control; std::string error;
        auto state = control.Order({{"live", {{"yaw", 0.5}, {"zoom", 99}, {"junk", 1}}}, {"free", true}}, error);
        Require(!state.is_null() && state["free"] == true && Near(state["live"]["yaw"], 0.5f) && Near(state["live"]["zoom"], MaxZoom), "Live offsets must be taken and limited");
        control.SetFrame(120);
        state = control.Order({{"addKey", {{"at", {{"yaw", 1}}}}}}, error);
        Require(state["keys"].size() == 1 && state["keys"][0]["frame"] == 120, "A key without a frame must land on the battle's frame");
        state = control.Order({{"dolly", true}}, error);
        Require(state["dolly"] == true, "Dolly must go on with a key");
        state = control.Order({{"clearKeys", true}}, error);
        Require(state["dolly"] == false && state["keys"].empty(), "Dolly must go off without keys");
        Require(control.Order({{"keys", nlohmann::json::array({{{"frame", 1}}, {{"frame", 2}}, 7})}, {"dolly", true}}, error)["keys"].size() == 3, "Keys must be taken, a stray value as a key at frame 0");
        nlohmann::json many = nlohmann::json::array();
        for (int i = 0; i < 100; ++i) many.push_back({{"frame", i}});
        Require(control.Order({{"keys", many}}, error).is_null() && !error.empty(), "Too many keys must be refused");
        Require(control.Order(5, error).is_null(), "A non-object order must be refused");
        state = control.Order({{"reset", true}}, error);
        Require(!control.Get().Active() && state["free"] == false, "Reset must give back the game's camera");
        // Render: needs two keys and a watched replay.
        control.Order({{"keys", nlohmann::json::array({{{"frame", 10}}, {{"frame", 50}}})}}, error);
        Require(control.Order({{"render", true}}, error).is_null() && !error.empty(), "A render outside a watched replay must be refused");
        control.SetRender(Render::Ready);
        Require(control.Order({{"clearKeys", true}, {"render", true}}, error).is_null(), "A render without keys must be refused");
        control.Order({{"keys", nlohmann::json::array({{{"frame", 10}}, {{"frame", 50}}})}}, error);
        state = control.Order({{"render", true}}, error);
        Require(!state.is_null() && state["dolly"] == true && state["render"] == 1 && control.TakeRender() && !control.TakeRender(), "A render must turn the dolly on and be taken once");
        // A glide eases the live offsets by the clock, holds, and a hand on the pad ends it.
        control.Order({{"reset", true}}, error);
        const auto t0 = Clock::now(); const auto after = [&](int ms) { return t0 + std::chrono::milliseconds(ms); };
        state = control.Order({{"glide", nlohmann::json::array({{{"at", {{"yaw", 1}}}, {"ms", 1000}, {"ease", 1}, {"hold", 500}}, {{"at", {{"yaw", 0}}}, {"ms", 1000}, {"ease", 0}}})}}, error, t0);
        Require(!state.is_null() && state["gliding"] == true, "A glide must start");
        Require(Near(control.Get(after(500)).live.yaw, 0.5f) && control.Get(after(500)).gliding, "Smooth must be half way at the middle");
        Require(Near(control.Get(after(1200)).live.yaw, 1.f), "A leg must hold its end");
        Require(Near(control.Get(after(2000)).live.yaw, 0.5f), "The next leg must start from the last end");
        Require(!control.Get(after(2600)).gliding && Near(control.Get(after(2600)).live.yaw, 0.f), "A glide must end on its last leg");
        control.Order({{"glide", nlohmann::json::array({{{"at", {{"yaw", 1}}}, {"ms", 1000}, {"ease", 0}}, {{"at", {{"yaw", 0}}}, {"ms", 1000}, {"ease", 0}}})}, {"loop", true}}, error, t0);
        Require(control.Get(after(5500)).gliding && Near(control.Get(after(5500)).live.yaw, 0.5f), "A loop must come round again");
        control.Order({{"live", {{"yaw", 0.25}}}}, error, after(5600));
        Require(!control.Get(after(9000)).gliding && Near(control.Get(after(9000)).live.yaw, 0.25f), "The pad must end a glide");
        Require(control.Order({{"glide", nlohmann::json::array()}}, error).is_null(), "An empty glide must be refused");
        // Saved views: kept across a reset, glided to, toured, and written to a file that a new Control reads.
        const auto file = std::filesystem::temp_directory_path() / "sf4e-camera-views-test.json";
        std::filesystem::remove(file);
        {
            Control saved; saved.SetFile(file);
            saved.Order({{"live", {{"yaw", 0.5}}}}, error);
            Require(saved.Order({{"saveView", 1}, {"viewName", "Side"}}, error)["views"][1]["name"] == "Side", "A view must be saved to its slot with its name");
            saved.Order({{"live", {{"zoom", 2}}}}, error);
            saved.Order({{"saveView", 2}}, error);
            Require(saved.Get().views[2].name == "View 3", "A view without a name must get its slot's");
            Require(saved.Order({{"saveView", 9}}, error).is_null() && !error.empty(), "A slot out of range must be refused");
            Require(saved.Order({{"goView", 0}}, error).is_null() && !error.empty(), "Going to an empty view must be refused");
            saved.Order({{"reset", true}, {"glideMs", 1000}}, error);
            Require(saved.Get().views[1].used && saved.Get().live.Identity() && saved.Get().glideMs == 1000, "Reset must keep the views and the glide time");
            const auto t = Clock::now();
            Require(saved.Order({{"goView", 1}}, error, t)["gliding"] == true, "Going to a view must start a glide");
            Require(Near(saved.Get(t + std::chrono::milliseconds(1500)).live.yaw, 0.5f), "A glide to a view must end on it");
            Require(saved.Order({{"tour", true}}, error, t)["gliding"] == true, "A tour of two views must start");
            saved.Order({{"deleteView", 2}}, error);
            Require(saved.Order({{"tour", true}}, error).is_null() && !error.empty(), "A tour needs two views");
            Require(saved.Order({{"effect", "push"}}, error, t)["gliding"] == true && Near(saved.Get(t + std::chrono::milliseconds(2000)).live.zoom, 0.5f), "Push in must halve the zoom");
            Require(saved.Order({{"effect", "nope"}}, error).is_null(), "An unknown effect must be refused");
            saved.Nudge({0.1f, 0, 1.5f, 0, 0, 0, 0}, t + std::chrono::milliseconds(2100));
            Require(!saved.Get(t + std::chrono::milliseconds(2100)).gliding && Near(saved.Get(t + std::chrono::milliseconds(2100)).live.zoom, 0.75f), "A nudge must end a glide and move by hand");
        }
        {
            Control again; again.SetFile(file);
            Require(again.Get().views[1].used && again.Get().views[1].name == "Side" && Near(again.Get().views[1].at.yaw, 0.5f) && !again.Get().views[2].used && again.Get().glideMs == 1000, "The views must come back from the file");
        }
        std::filesystem::remove(file);
        {
            // The timeline's key edits.
            Control tl; std::string error;
            tl.SetFrame(100); tl.Order({{"addKey", {{"at", {{"yaw", 0.1}}}}}}, error);
            tl.SetFrame(300); tl.Order({{"addKey", {{"at", {{"yaw", 0.9}}}}}}, error);
            tl.SetFrame(200); tl.Order({{"addKey", {{"at", {{"yaw", 0.5}}}}}}, error);
            Require(tl.Get().keys.size() == 3 && tl.Get().selected == 1 && tl.Get().keys[1].frame == 200, "An added key must be sorted in and selected");
            tl.Order({{"moveKey", {{"key", 1}, {"frame", 400}}}}, error);
            Require(tl.Get().keys[2].frame == 400 && tl.Get().selected == 2, "A moved key must re-sort and stay selected");
            tl.Order({{"live", {{"yaw", 0.7}}}, {"updateKey", 0}}, error);
            Require(Near(tl.Get().keys[0].at.yaw, 0.7f), "Update key must take the live offsets");
            tl.Order({{"easeKey", {{"key", 0}, {"ease", 2}}}}, error);
            Require(tl.Get().keys[0].ease == Ease::Hold, "Ease must be set on a key");
            Require(tl.Order({{"delKey", 9}}, error).is_null() && !error.empty(), "A key that is not there must be refused");
            tl.Order({{"delKey", 0}}, error);
            Require(tl.Get().keys.size() == 2 && tl.Get().selected == -1, "Delete key must drop it and the selection");
            int frame = -1;
            tl.Order({{"seek", 150}}, error);
            Require(tl.TakeSeek(frame) && frame == 150 && !tl.TakeSeek(frame), "A seek must be taken once");
            tl.SetGoal(1, 150); int round = 0;
            Require(tl.Goal(round, frame) && round == 1 && frame == 150, "The goal must stay until cleared");
            tl.AskSeek(10, true); bool play = false;
            Require(tl.TakeSeek(frame, play) && frame == 10 && play, "A seek must carry whether the replay plays on");
            tl.Order({{"loop", true}}, error);
            Require(tl.Get().loop, "The loop must turn on");
            tl.ClearGoal();
            Require(!tl.Goal(round, frame) && tl.GetSeek() == Seek::None, "A cleared goal must be gone");
        }
        {
            // Where the camera is in use: the overlay's offer (Training, a replay Ember plays) or a shot rendering; never over the network.
            Require(UseOf(true, Render::Unavailable, false) == Use::On && UseOf(false, Render::Recording, false) == Use::On && UseOf(false, Render::Leaving, false) == Use::On,
                "Training, a watched replay and a shot rendering must use the camera");
            Require(UseOf(false, Render::Ready, false) == Use::Off && UseOf(false, Render::Done, false) == Use::Off && UseOf(false, Render::Unavailable, false) == Use::Off,
                "Anywhere else the camera must be out of use");
            Require(UseOf(true, Render::Ready, true) == Use::Network && UseOf(false, Render::Recording, true) == Use::Network, "A battle played over the network must never use the camera");
        }
        {
            // The hook's layer: out of use it writes nothing, in use it lays over and puts back, and over the network it never writes.
            Control control; Layer layer; std::string error;
            control.Order({{"live", {{"yaw", 0.5}, {"zoom", 0.5}}}, {"free", true}}, error);
            FakeCamera camera; const FakeCamera game = camera; const Fields at = camera.At();
            layer.Restore(&at, Use::Off); layer.Lay(&at, Use::Off, control);
            Require(camera == game && !layer.Holding(), "A camera out of use must be left untouched");
            layer.Restore(&at, Use::Network); layer.Lay(&at, Use::Network, control);
            Require(camera == game && !layer.Holding(), "A battle played over the network must be left untouched");
            const auto check = layer.Lay(&at, Use::On, control);
            Require(check.made && check.trusted && layer.Holding() && !(camera == game) && !Near(camera.eye[0], game.eye[0]), "A camera in use must be laid over");
            layer.Restore(&at, Use::On);
            Require(camera == game && !layer.Holding(), "Before the next update the game's camera must be put back exactly");
            layer.Lay(&at, Use::On, control);
            const FakeCamera laid = camera;
            layer.Restore(&at, Use::Network); layer.Lay(&at, Use::Network, control);
            Require(camera == laid && !layer.Holding(), "Over the network what was held must be dropped, never written");
            camera = game;
            layer.Lay(&at, Use::On, control); layer.Lay(&at, Use::Off, control);
            Require(camera == game && !layer.Holding(), "Going out of use must put the game's camera back once");
            layer.Lay(&at, Use::Off, control);
            Require(camera == game, "Out of use nothing more is written");
            Require(!layer.Lay(nullptr, Use::On, control).made && !layer.Holding(), "No camera, nothing laid");
        }
        {
            // Training or the replay closing gives the game its camera back; the views and the shot's keys stay.
            Control control; std::string error;
            control.SetOffered(true);
            Require(control.TakesOrders(), "Offered, the camera must take orders");
            control.Order({{"live", {{"yaw", 0.5}}}, {"free", true}, {"saveView", 0}, {"keys", nlohmann::json::array({{{"frame", 10}}, {{"frame", 50}}})}, {"dolly", true}}, error);
            Require(control.Get().Active() && control.Get().dolly, "The settings must be taken");
            control.SetOffered(false);
            const auto rested = control.Get();
            Require(!rested.Active() && !rested.free && !rested.dolly && !rested.gliding && rested.keys.size() == 2 && rested.views[0].used, "Closing must rest the live settings and keep the views and keys");
            Require(!control.TakesOrders() && control.UseFor(false) == Use::Off, "Closed, the camera must refuse orders");
            control.SetOffered(true); control.Order({{"dolly", true}}, error);
            control.SetRender(Render::Waiting); control.SetOffered(false);
            Require(control.Get().dolly && control.TakesOrders() && control.UseFor(true) == Use::Network, "A shot rendering must keep its keys driving the camera");
            control.SetRender(Render::Done); control.SetOffered(false);
            Require(!control.Get().dolly && !control.TakesOrders(), "The shot done, the camera must rest");
        }
        {
            // An order's fields of the wrong type count as not there: nothing throws, nothing is narrowed.
            Control control; std::string error;
            control.SetFrame(10); control.Order({{"addKey", {{"frame", 5}}}}, error);
            auto state = control.Order({{"reset", 1}, {"stopGlide", "x"}, {"free", "yes"}, {"glideMs", 1e300}, {"loop", 2}, {"tour", "y"}, {"render", 1}, {"clearKeys", 1}}, error);
            Require(!state.is_null() && state["glideMs"] == MaxLegMs && state["free"] == false && state["keys"].size() == 1, "Fields of the wrong type must be skipped and numbers kept in range");
            Require(control.Order({{"easeKey", {{"key", "0"}, {"ease", "x"}}}}, error).is_null() && error == "bad key", "A key index that is not a number must be refused");
            Require(control.Order({{"selectKey", 4294967296.0}}, error).is_null() && control.Order({{"delKey", 4294967296LL}}, error).is_null(), "A key index past an int must be refused, not cut down");
            state = control.Order({{"moveKey", {{"key", 0}, {"frame", "z"}}}, {"easeKey", {{"key", 0}, {"ease", "x"}}}}, error);
            Require(!state.is_null() && state["keys"][0]["frame"] == 5 && state["keys"][0]["ease"] == static_cast<int>(Ease::Smooth), "A key's other fields of the wrong type must keep their values");
            state = control.Order({{"saveView", 0}, {"viewName", 5}, {"seek", 1e30}, {"addKey", 7}}, error);
            Require(!state.is_null() && state["views"][0]["name"] == "View 1" && state["keys"].size() == 2, "A name that is not text must give the slot's own");
            Require(!control.Order({{"glide", nlohmann::json::array({{{"ms", "x"}, {"hold", 1e300}, {"at", 3}}})}}, error).is_null(), "A glide's legs of the wrong type must be skipped");
            Require(control.Order({{"saveView", "0"}}, error).is_null() && error == "bad slot", "A slot that is not a number must be refused");
            // A long name is cut between UTF-8 characters: 39 bytes, then a three-byte character that does not fit.
            state = control.Order({{"saveView", 1}, {"viewName", std::string(39, 'a') + "\xE3\x83\xAA"}}, error);
            Require(state["views"][1]["name"] == std::string(39, 'a'), "A name must be cut between characters");
        }
        {
            // A views file of the wrong shape is read for what it holds and nothing else.
            const auto file = std::filesystem::temp_directory_path() / "sf4e-camera-views-wrong.json";
            std::string longName;
            for (int i = 0; i < 20; ++i) longName += "\xE3\x83\xAA";  // 60 bytes of three-byte characters
            std::ofstream(file, std::ios::binary) << "{\"glideMs\":\"x\",\"views\":[{\"used\":1,\"name\":\"A\"},{\"used\":true,\"name\":[\"a\"],\"at\":7},"
                "{\"used\":true,\"name\":\"" << longName << "\",\"at\":{\"yaw\":\"q\",\"zoom\":2}}]}";
            Control control; control.SetFile(file);
            const auto s = control.Get();
            Require(s.glideMs == 2000 && !s.views[0].used && s.views[1].used && s.views[1].name.empty() && s.views[1].at.Identity(), "Fields of the wrong type in the file must be skipped");
            Require(s.views[2].used && s.views[2].name.size() <= ViewNameBytes && sf4e::IsWellFormedUtf8(s.views[2].name) && Near(s.views[2].at.zoom, 2), "A name in the file must come out as UTF-8 within its length");
            Require(!nlohmann::json(control.State()).dump().empty(), "The state with the file's names must be written");
            std::filesystem::remove(file);
        }
        // The view matrix of a camera looking along +z from the origin is the identity.
        float view[16]; ViewMatrix(Pose{{0, 0, 0}, {0, 0, 1}, 0, 0.7854f}, view);
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        Require(MaxDifference(view, identity) < 1e-5f, "A camera at the origin looking down +z must have the identity view");
        std::puts("camera rig ok");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
