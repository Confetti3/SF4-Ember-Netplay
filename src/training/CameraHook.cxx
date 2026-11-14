#include "CameraHook.hxx"
#include "CameraLayer.hxx"
#include <spdlog/spdlog.h>
#include "../Dimps/Dimps__Game__Battle__Camera.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../sf4e/sf4e__Game__Battle__System.hxx"

namespace sf4e { namespace training { namespace camera {
namespace {
using Native = Dimps::Game::Battle::System;
using CamUnit = Dimps::Game::Battle::Camera::Unit;
Layer s_layer;
// The battle's camera, when it has one: the active camera object is at Camera::Unit +0x130.
bool Current(Native* system, Fields& f) {
    auto* unit = reinterpret_cast<CamUnit*>((system->*Native::publicMethods.GetUnitByIndex)(Native::U_CAMERA));
    char* cam = unit ? *reinterpret_cast<char**>(reinterpret_cast<char*>(unit) + 0x130) : nullptr;
    if (!cam) return false;
    f = {reinterpret_cast<float*>(cam + 0x10), reinterpret_cast<float*>(cam + 0x50), reinterpret_cast<float*>(cam + 0x90),
        reinterpret_cast<float*>(cam + 0xa0), reinterpret_cast<float*>(cam + 0xb0), reinterpret_cast<float*>(cam + 0xd4), reinterpret_cast<float*>(cam + 0xd8),
        CamUnit::GetViewMatrix(unit), CamUnit::GetProjectionMatrix(unit)};
    return true;
}
// Whether the camera may touch this battle: the overlay's offer, and never a battle GGPO plays.
Use UseNow() { return Phone().UseFor(sf4e::Game::Battle::System::ggpo != nullptr); }
}

void Restore(Native* system) {
    Fields f;
    const bool have = Current(system, f);
    s_layer.Restore(have ? &f : nullptr, UseNow());
}

void Apply(Native* system) {
    Control& control = Phone();
    Fields f;
    const bool have = Current(system, f);
    control.SetFrame(have ? Native::GetNumFramesSimulated_FixedPoint(system)->integral : -1);
    const Layer::Check check = s_layer.Lay(have ? &f : nullptr, UseNow(), control);
    if (check.made)
        spdlog::info("Camera: view matrix maths {} the game's (largest difference {:.5f}); eye {:.3f} {:.3f} {:.3f}, target {:.3f} {:.3f} {:.3f}, roll {:.3f}, fov {:.4f}",
            check.trusted ? "matches" : "does NOT match, camera left alone:", check.difference, check.game.eye.x, check.game.eye.y, check.game.eye.z,
            check.game.target.x, check.game.target.y, check.game.target.z, check.game.roll, check.game.fov);
}
} } }
