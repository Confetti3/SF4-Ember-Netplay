#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// The phone's camera: offsets laid over the camera the game computed, and a
// dolly track of such offsets over replay frames. Pure; the game's camera is
// read and written by CameraHook.cxx.
// Orbit turns the eye round the target (yaw about the world's up, pitch about
// the camera's right), zoom scales the distance, pan slides eye and target
// together across the picture, roll and fov add to the game's.
namespace sf4e { namespace training { namespace camera {
struct Vec3 { float x = 0, y = 0, z = 0; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline Vec3 Unit(Vec3 a) { const float n = std::sqrt(Dot(a, a)); return n > 1e-6f ? a * (1 / n) : Vec3{0, 0, 1}; }
// Turns v about a unit axis by angle (Rodrigues).
inline Vec3 Turn(Vec3 v, Vec3 axis, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    return v * c + Cross(axis, v) * s + axis * (Dot(axis, v) * (1 - c));
}
// A camera as the game holds it: eye, target, roll and vertical FOV in radians.
struct Pose { Vec3 eye, target; float roll = 0, fov = 0.7854f; };
// Radians, a factor and world units. All zero except zoom 1 is the game's camera.
struct Offsets {
    float yaw = 0, pitch = 0, zoom = 1, panX = 0, panY = 0, roll = 0, fov = 0;
    bool Identity() const { return yaw == 0 && pitch == 0 && zoom == 1 && panX == 0 && panY == 0 && roll == 0 && fov == 0; }
};
constexpr float MaxPitch = 1.45f, MinZoom = 0.2f, MaxZoom = 4.f, MaxFov = 1.6f, MinFov = 0.15f, MaxPan = 6.f;
inline float Clamp(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }
// The offsets kept to what looks sensible; NaN becomes the neutral value.
inline Offsets Limit(Offsets o) {
    const auto finite = [](float v, float neutral) { return std::isfinite(v) ? v : neutral; };
    o.yaw = finite(o.yaw, 0); o.pitch = Clamp(finite(o.pitch, 0), -MaxPitch, MaxPitch); o.zoom = Clamp(finite(o.zoom, 1), MinZoom, MaxZoom);
    o.panX = Clamp(finite(o.panX, 0), -MaxPan, MaxPan); o.panY = Clamp(finite(o.panY, 0), -MaxPan, MaxPan);
    o.roll = finite(o.roll, 0); o.fov = Clamp(finite(o.fov, 0), -0.6f, 0.8f);
    return o;
}
inline Pose Apply(const Pose& game, Offsets o) {
    o = Limit(o);
    if (o.Identity()) return game;
    Pose pose = game;
    Vec3 arm = game.eye - game.target;
    const Vec3 up{0, 1, 0};
    arm = Turn(arm, up, o.yaw);
    const Vec3 right = Unit(Cross(up, arm * -1.f));  // the camera looks along -arm
    // Pitching past the pole would flip the picture; the arm's own elevation bounds it.
    const float length = std::sqrt(Dot(arm, arm)), elevation = std::asin(Clamp(arm.y / (length > 1e-6f ? length : 1), -1, 1));
    arm = Turn(arm, right, Clamp(elevation + o.pitch, -MaxPitch, MaxPitch) - elevation);
    arm = arm * o.zoom;
    const Vec3 look = Unit(arm * -1.f), camRight = Unit(Cross(up, look)), camUp = Cross(look, camRight);
    const Vec3 slide = camRight * o.panX + camUp * o.panY;
    pose.target = game.target + slide;
    pose.eye = pose.target + arm;
    pose.roll = game.roll + o.roll;
    pose.fov = Clamp(game.fov + o.fov, MinFov, MaxFov);
    return pose;
}
// The view matrix the game builds from a pose (D3D left-handed look-at, rows
// first, the translation in the fourth row), and the projection's two scales
// for a vertical FOV and aspect. CameraHook.cxx checks the first against the
// game's own matrix before it changes anything.
inline void ViewMatrix(const Pose& p, float (&m)[16]) {
    const Vec3 z = Unit(p.target - p.eye);
    Vec3 x = Unit(Cross(Vec3{0, 1, 0}, z));
    Vec3 y = Cross(z, x);
    const float c = std::cos(p.roll), sn = std::sin(p.roll);
    const Vec3 rx = x * c + y * sn, ry = y * c - x * sn;  // roll turns the picture about the view axis
    x = rx; y = ry;
    const float v[16] = {x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -Dot(x, p.eye), -Dot(y, p.eye), -Dot(z, p.eye), 1};
    for (int i = 0; i < 16; ++i) m[i] = v[i];
}
// m[5] is 1 / tan(fov / 2) and m[0] that over the aspect; the rest of the game's projection stays.
inline void ProjectionScales(float fov, float aspect, float& m0, float& m5) { m5 = 1 / std::tan(fov / 2); m0 = m5 / aspect; }
inline float MaxDifference(const float* a, const float* b) {
    float most = 0;
    for (int i = 0; i < 16; ++i) most = (std::max)(most, std::fabs(a[i] - b[i]));
    return most;
}
// A dolly shot: offsets at replay frames, eased between. Frames ascend.
enum class Ease { Linear, Smooth, Hold };
struct Key { int frame = 0; Offsets at; Ease ease = Ease::Smooth; };
inline Offsets Mix(const Offsets& a, const Offsets& b, float t) {
    const auto m = [t](float x, float y) { return x + (y - x) * t; };
    return {m(a.yaw, b.yaw), m(a.pitch, b.pitch), m(a.zoom, b.zoom), m(a.panX, b.panX), m(a.panY, b.panY), m(a.roll, b.roll), m(a.fov, b.fov)};
}
// The offsets at a frame: the first key's before it, the last's after it, and
// between two keys the earlier key's ease decides how the later is approached.
inline Offsets Sample(const std::vector<Key>& keys, int frame) {
    if (keys.empty()) return {};
    if (frame <= keys.front().frame) return keys.front().at;
    if (frame >= keys.back().frame) return keys.back().at;
    std::size_t i = 1;
    while (keys[i].frame < frame) ++i;
    const Key &from = keys[i - 1], &to = keys[i];
    if (from.ease == Ease::Hold) return from.at;
    float t = static_cast<float>(frame - from.frame) / static_cast<float>(to.frame - from.frame);
    if (from.ease == Ease::Smooth) t = t * t * (3 - 2 * t);
    return Mix(from.at, to.at, t);
}
// Keys sorted, one per frame (a later one replaces), none below frame 0.
inline void Tidy(std::vector<Key>& keys) {
    keys.erase(std::remove_if(keys.begin(), keys.end(), [](const Key& k) { return k.frame < 0; }), keys.end());
    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.frame < b.frame; });
    std::vector<Key> out;
    for (const auto& k : keys) { if (!out.empty() && out.back().frame == k.frame) out.back() = k; else out.push_back(k); }
    keys = std::move(out);
}
} } }
