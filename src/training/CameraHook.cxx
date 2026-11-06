#include "CameraHook.hxx"
#include "CameraControl.hxx"
#include <cstring>
#include <spdlog/spdlog.h>
#include "../Dimps/Dimps__Game__Battle__Camera.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"

namespace sf4e { namespace training { namespace camera {
namespace {
using Native = Dimps::Game::Battle::System;
using CamUnit = Dimps::Game::Battle::Camera::Unit;
// The active camera object (Camera::Unit +0x130; notes: battle_presentation.md 5.1):
// view +0x10, projection +0x50, eye +0x90, target +0xa0, roll +0xb0, aspect +0xd4, FOV +0xd8.
// The unit holds the matrices the picture is drawn through at +0x10 and +0x90.
struct Fields {
    float* view; float* proj; float* eye; float* target; float* roll; float* aspect; float* fov;
    float* unitView; float* unitProj;
};
Fields Locate(CamUnit* unit, char* cam) {
    return {reinterpret_cast<float*>(cam + 0x10), reinterpret_cast<float*>(cam + 0x50), reinterpret_cast<float*>(cam + 0x90),
        reinterpret_cast<float*>(cam + 0xa0), reinterpret_cast<float*>(cam + 0xb0), reinterpret_cast<float*>(cam + 0xd4), reinterpret_cast<float*>(cam + 0xd8),
        CamUnit::GetViewMatrix(unit), CamUnit::GetProjectionMatrix(unit)};
}
Pose ReadPose(const Fields& f) {
    return {{f.eye[0], f.eye[1], f.eye[2]}, {f.target[0], f.target[1], f.target[2]}, *f.roll, *f.fov};
}
struct Saved {
    bool applied = false;
    CamUnit* unit = nullptr; char* cam = nullptr;
    float eye[3], target[3], roll, fov, view[16], proj[16], unitView[16], unitProj[16];
    // Whether the matrix maths agreed with the game's for this camera (checked once per camera); false: touch nothing.
    bool checked = false, trusted = false;
    bool frozen = false; Pose freeze;
} s;
bool Current(Native* system, CamUnit*& unit, char*& cam) {
    unit = reinterpret_cast<CamUnit*>((system->*Native::publicMethods.GetUnitByIndex)(Native::U_CAMERA));
    cam = unit ? *reinterpret_cast<char**>(reinterpret_cast<char*>(unit) + 0x130) : nullptr;
    return unit && cam;
}
void Put(float* to, const float* from, int count) { std::memcpy(to, from, count * sizeof(float)); }
}

void Restore(Native* system) {
    CamUnit* unit; char* cam;
    if (s.applied && Current(system, unit, cam) && unit == s.unit && cam == s.cam) {
        const Fields f = Locate(unit, cam);
        Put(f.eye, s.eye, 3); Put(f.target, s.target, 3); *f.roll = s.roll; *f.fov = s.fov;
        Put(f.view, s.view, 16); Put(f.proj, s.proj, 16); Put(f.unitView, s.unitView, 16); Put(f.unitProj, s.unitProj, 16);
    }
    s.applied = false;
}

void Apply(Native* system) {
    Control& control = Phone();
    CamUnit* unit; char* cam;
    const bool have = Current(system, unit, cam);
    control.SetFrame(have ? Native::GetNumFramesSimulated_FixedPoint(system)->integral : -1);
    if (!have) { s = Saved{}; return; }
    Restore(system);  // a frame the game did not update is laid over from its own camera again
    if (unit != s.unit || cam != s.cam) { s = Saved{}; s.unit = unit; s.cam = cam; }
    const Settings settings = control.Get();
    if (!settings.free) s.frozen = false;
    if (!settings.Active()) return;
    const Fields f = Locate(unit, cam);
    const Pose game = ReadPose(f);
    if (!s.checked) {
        s.checked = true;
        float mine[16]; ViewMatrix(game, mine);
        const float off = MaxDifference(mine, f.view);
        s.trusted = off < 0.01f;
        spdlog::info("Camera: view matrix maths {} the game's (largest difference {:.5f}); eye {:.3f} {:.3f} {:.3f}, target {:.3f} {:.3f} {:.3f}, roll {:.3f}, fov {:.4f}",
            s.trusted ? "matches" : "does NOT match, camera left alone:", off, game.eye.x, game.eye.y, game.eye.z, game.target.x, game.target.y, game.target.z, game.roll, game.fov);
    }
    if (!s.trusted) return;
    Pose base = game;
    if (settings.free) { if (!s.frozen) { s.freeze = game; s.frozen = true; } base = s.freeze; }
    const Offsets offsets = settings.dolly ? Sample(settings.keys, control.Frame()) : settings.live;
    const Pose pose = Apply(base, offsets);
    Put(s.eye, f.eye, 3); Put(s.target, f.target, 3); s.roll = *f.roll; s.fov = *f.fov;
    Put(s.view, f.view, 16); Put(s.proj, f.proj, 16); Put(s.unitView, f.unitView, 16); Put(s.unitProj, f.unitProj, 16);
    s.applied = true;
    const float eye[3] = {pose.eye.x, pose.eye.y, pose.eye.z}, target[3] = {pose.target.x, pose.target.y, pose.target.z};
    Put(f.eye, eye, 3); Put(f.target, target, 3); *f.roll = pose.roll; *f.fov = pose.fov;
    float view[16], proj[16]; ViewMatrix(pose, view);
    Put(proj, f.proj, 16); ProjectionScales(pose.fov, *f.aspect, proj[0], proj[5]);
    Put(f.view, view, 16); Put(f.unitView, view, 16); Put(f.proj, proj, 16); Put(f.unitProj, proj, 16);
}
} } }
