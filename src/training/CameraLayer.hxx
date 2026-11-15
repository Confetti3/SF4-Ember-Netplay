#pragma once
#include "CameraControl.hxx"
#include <cstring>

// The game's camera with the camera's settings laid over it for one frame's
// picture. Laid over after the game's update and taken off before the next, so
// the game's own state (its rollback saves among them) never holds the offsets.
// CameraHook.cxx finds the fields in the battle's camera unit; pure, so the
// tests lay it over plain arrays.
namespace sf4e { namespace training { namespace camera {
// The active camera object's view +0x10, projection +0x50, eye +0x90, target
// +0xa0, roll +0xb0, aspect +0xd4 and FOV +0xd8, and the matrices the unit draws
// the picture through (notes: battle_presentation.md 5.1).
struct Fields {
    float* view; float* proj; float* eye; float* target; float* roll; float* aspect; float* fov;
    float* unitView; float* unitProj;
    // The same camera: the same camera object in the same unit.
    bool Same(const Fields& other) const { return eye == other.eye && unitView == other.unitView; }
};
inline Pose ReadPose(const Fields& f) { return {{f.eye[0], f.eye[1], f.eye[2]}, {f.target[0], f.target[1], f.target[2]}, *f.roll, *f.fov}; }

class Layer {
public:
    // The first comparison of the matrix maths with the game's own for a camera; until it
    // matches, the camera is left alone.
    struct Check { bool made = false, trusted = false; float difference = 0; Pose game; };
    // Before an update: puts back what Lay changed, while it is the same camera. Over a
    // battle played over the network nothing is written: what is held belongs to
    // another battle, and it is dropped.
    void Restore(const Fields* now, Use use) {
        if (held_.applied && use != Use::Network && now && now->Same(held_.at)) {
            Put(now->eye, held_.eye, 3); Put(now->target, held_.target, 3); *now->roll = held_.roll; *now->fov = held_.fov;
            Put(now->view, held_.view, 16); Put(now->proj, held_.proj, 16); Put(now->unitView, held_.unitView, 16); Put(now->unitProj, held_.unitProj, 16);
        }
        held_.applied = false;
    }
    // After an update: lays the settings over the camera (`now`: none while the battle has
    // none). Out of use, the game's camera is put back once and nothing more is held.
    Check Lay(const Fields* now, Use use, const Control& control) {
        Restore(now, use);  // a frame the game did not update is laid over from its own camera again
        if (!now || use != Use::On) { held_ = Held{}; return {}; }
        if (!held_.known || !now->Same(held_.at)) { held_ = Held{}; held_.known = true; held_.at = *now; }
        const Settings settings = control.Get();
        if (!settings.free) held_.frozen = false;
        if (!settings.Active()) return {};
        const Pose game = ReadPose(*now);
        Check check;
        if (!held_.checked) {
            float mine[16]; ViewMatrix(game, mine);
            check.made = true; check.difference = MaxDifference(mine, now->view); check.trusted = check.difference < 0.01f; check.game = game;
            held_.checked = true; held_.trusted = check.trusted;
        }
        if (!held_.trusted) return check;
        Pose base = game;
        if (settings.free) { if (!held_.frozen) { held_.freeze = game; held_.frozen = true; } base = held_.freeze; }
        const Pose pose = Apply(base, settings.dolly ? Sample(settings.keys, control.Frame()) : settings.live);
        Put(held_.eye, now->eye, 3); Put(held_.target, now->target, 3); held_.roll = *now->roll; held_.fov = *now->fov;
        Put(held_.view, now->view, 16); Put(held_.proj, now->proj, 16); Put(held_.unitView, now->unitView, 16); Put(held_.unitProj, now->unitProj, 16);
        held_.applied = true;
        const float eye[3] = {pose.eye.x, pose.eye.y, pose.eye.z}, target[3] = {pose.target.x, pose.target.y, pose.target.z};
        Put(now->eye, eye, 3); Put(now->target, target, 3); *now->roll = pose.roll; *now->fov = pose.fov;
        float view[16], proj[16]; ViewMatrix(pose, view);
        Put(proj, now->proj, 16); ProjectionScales(pose.fov, *now->aspect, proj[0], proj[5]);
        Put(now->view, view, 16); Put(now->unitView, view, 16); Put(now->proj, proj, 16); Put(now->unitProj, proj, 16);
        return check;
    }
    // Whether it holds a camera it changed, to be put back before the next update.
    bool Holding() const { return held_.applied; }
private:
    struct Held {
        bool known = false; Fields at{};
        bool applied = false;
        float eye[3] = {}, target[3] = {}, roll = 0, fov = 0, view[16] = {}, proj[16] = {}, unitView[16] = {}, unitProj[16] = {};
        bool checked = false, trusted = false;
        bool frozen = false; Pose freeze;
    };
    static void Put(float* to, const float* from, int count) { std::memcpy(to, from, count * sizeof(float)); }
    Held held_;
};
} } }
