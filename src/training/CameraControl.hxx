#pragma once
#include "CameraRig.hxx"
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

// The camera's settings, shared between the phone's thread (which sets them)
// and the game thread (which reads them each frame), and the JSON order that
// changes them. Pure; the game's camera is CameraHook.cxx.
namespace sf4e { namespace training { namespace camera {
constexpr std::size_t MaxKeys = 64;
// Where "render shot" stands (sf4e__ReplayStore.cxx moves it): a shot is rendered from a replay
// Ember started; it leaves that replay, plays it again as an export and records only the keys' frames.
enum class Render { Unavailable, Ready, Leaving, Starting, Waiting, Recording, Closing, Done, Failed };
// One move of a glide: ease the live offsets to `to` over ms, then wait hold ms.
struct Leg { Offsets to; int ms = 1000; int hold = 0; Ease ease = Ease::Smooth; };
constexpr int MaxLegs = 32, MinLegMs = 50, MaxLegMs = 120000;
using Clock = std::chrono::steady_clock;
// A camera the player saved to come back to, and the slots there are (more later).
struct View { bool used = false; std::string name; Offsets at; };
constexpr int ViewSlots = 4;
// Where a jump along a replay stands (sf4e__ReplayPlayback.cxx moves it): a jump ahead is played to at speed, a jump back
// loads a snapshot and then plays to the frame.
enum class Seek { None, Forward };
struct Settings {
    std::array<View, ViewSlots> views;
    int glideMs = 2000;         // how long a glide to a view or an effect takes
    bool gliding = false;       // legs are moving `live` by the clock
    Offsets live;               // what the pad on the phone holds now
    bool free = false;          // freeze the game's camera where it was when this went on; the offsets move that
    bool dolly = false;         // follow the keys by the battle's frame instead of the live offsets
    std::vector<Key> keys;
    int selected = -1;          // the key the timeline has picked (an index), or -1
    bool loop = false;          // a replay plays the span from the picked key to the next over and over
    bool Active() const { return free || dolly || !live.Identity(); }
};
class Control {
public:
    // `live` is where a glide has got to by `now`.
    Settings Get(Clock::time_point now = Clock::now()) const { std::lock_guard<std::mutex> hold(lock_); Advance(now); return settings_; }
    // The order, applied; the answer to send back. Keys are kept sorted, at most MaxKeys.
    nlohmann::json Order(const nlohmann::json& body, std::string& error, Clock::time_point now = Clock::now());
    nlohmann::json State() const { auto j = Json(Get()); j["frame"] = frame_.load(); j["render"] = render_.load(); j["seek"] = static_cast<int>(seek_.load()); j["total"] = total_.load(); return j; }
    // The saved views and the glide time live in this file: read now, written when they change.
    void SetFile(std::filesystem::path file);
    // Moves the live offsets by hand (a held key or stick): yaw, pitch, pans, roll and fov add, zoom multiplies. Ends a glide.
    void Nudge(const Offsets& by, Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> hold(lock_);
        Advance(now);
        legs_.clear(); settings_.gliding = false;
        Offsets& o = settings_.live;
        o.yaw += by.yaw; o.pitch += by.pitch; o.zoom *= by.zoom; o.panX += by.panX; o.panY += by.panY; o.roll += by.roll; o.fov += by.fov;
        o = Limit(o);
    }
    void Reset() { std::lock_guard<std::mutex> hold(lock_); Settings fresh; fresh.views = settings_.views; fresh.glideMs = settings_.glideMs; settings_ = fresh; legs_.clear(); }
    static nlohmann::json Json(const Settings& s);
    // The battle's frame as the game thread last saw it (-1: not in a fight); a key added without a frame lands here.
    void SetFrame(int frame) { frame_ = frame; }
    int Frame() const { return frame_; }
    // The phone asks for a render, or to stop waiting for one; the game thread takes each once.
    void AskRender() { renderAsked_ = true; }
    bool TakeRender() { return renderAsked_.exchange(false); }
    void AskCancelRender() { cancelAsked_ = true; }
    bool TakeCancelRender() { return cancelAsked_.exchange(false); }
    void SetRender(Render state) { render_ = static_cast<int>(state); }
    // A jump along the replay: the timeline asks for a frame of the current round, the game thread takes the ask once and
    // then holds the goal (round, frame) until the replay has played to it. The goal outlives the restart a jump back needs.
    // `play`: the replay goes on playing there (a loop); otherwise it waits at the frame.
    void AskSeek(int frame, bool play = false) { seekFrame_ = (std::max)(frame, 0); seekPlay_ = play; seekAsked_ = true; }
    bool TakeSeek(int& frame, bool& play) { if (!seekAsked_.exchange(false)) return false; frame = seekFrame_; play = seekPlay_; return true; }
    bool TakeSeek(int& frame) { bool play; return TakeSeek(frame, play); }
    void SetGoal(int round, int frame, bool play = false) { std::lock_guard<std::mutex> hold(goalLock_); goalRound_ = round; goalFrame_ = frame; goalPlay_ = play; goal_ = true; }
    bool GoalPlay() const { std::lock_guard<std::mutex> hold(goalLock_); return goalPlay_; }
    void ClearGoal() { std::lock_guard<std::mutex> hold(goalLock_); goal_ = false; seek_ = static_cast<int>(Seek::None); }
    bool Goal(int& round, int& frame) const { std::lock_guard<std::mutex> hold(goalLock_); round = goalRound_; frame = goalFrame_; return goal_; }
    void SetSeek(Seek state) { seek_ = static_cast<int>(state); }
    Seek GetSeek() const { return static_cast<Seek>(seek_.load()); }
    // The earliest frame a jump back can reach: the first snapshot of the round (the controls take the playback over at the fight, not at frame 0).
    void SetFloor(int frame) { floor_ = frame; }
    int Floor() const { return floor_; }
    // The frames of the replay's round now playing (0 outside a replay).
    void SetTotal(int frames) { total_ = frames; }
    int Total() const { return total_; }
    Render GetRender() const { return static_cast<Render>(render_.load()); }
private:
    // Moves the glide on to `now`; the lock is held.
    void Advance(Clock::time_point now) const {
        while (!legs_.empty()) {
            const Leg& leg = legs_[at_];
            const long long since = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count();
            const int elapsed = static_cast<int>((std::min)(since, 1000000000LL));
            if (elapsed < 0) { settings_.live = from_; return; }
            if (elapsed < leg.ms) {
                float t = static_cast<float>(elapsed) / static_cast<float>(leg.ms);
                if (leg.ease == Ease::Smooth) t = t * t * (3 - 2 * t);
                settings_.live = Mix(from_, leg.to, t);
                return;
            }
            settings_.live = leg.to;
            if (elapsed < leg.ms + leg.hold) return;
            from_ = leg.to; start_ += std::chrono::milliseconds(leg.ms + leg.hold);
            if (++at_ == legs_.size()) { at_ = 0; if (!loop_) { legs_.clear(); settings_.live.yaw = std::remainder(settings_.live.yaw, 6.2831853f); } }
        }
        settings_.gliding = false;
    }
    std::atomic<bool> renderAsked_{false}, cancelAsked_{false};
    std::atomic<int> render_{0};
    std::atomic<int> frame_{-1}, total_{0}, seek_{0}, seekFrame_{0}, floor_{0};
    std::atomic<bool> seekAsked_{false}, seekPlay_{false};
    mutable std::mutex goalLock_;
    bool goal_ = false, goalPlay_ = false;
    int goalRound_ = 0, goalFrame_ = 0;
    mutable std::mutex lock_;
    mutable Settings settings_;
    mutable std::vector<Leg> legs_;
    mutable std::size_t at_ = 0;
    mutable Offsets from_;
    mutable Clock::time_point start_;
    bool loop_ = false;
    std::filesystem::path file_;
    void Save() const {
        if (file_.empty()) return;
        std::error_code ignored;
        std::filesystem::create_directories(file_.parent_path(), ignored);
        std::ofstream out(file_);
        if (out) out << nlohmann::json{{"glideMs", settings_.glideMs}, {"views", ViewsJson(settings_)}}.dump(1);
    }
    static nlohmann::json ViewsJson(const Settings& s);
    void StartGlide(std::vector<Leg> legs, bool loop, Clock::time_point now) {
        legs_ = std::move(legs); loop_ = loop;
        at_ = 0; from_ = settings_.live; start_ = now; settings_.gliding = true;
    }
};
// The legs of a named effect from where the camera is; false for a name there is none of.
inline bool EffectLegs(const std::string& name, const Offsets& now, int ms, std::vector<Leg>& legs) {
    Leg leg; leg.ms = ms; leg.to = now;
    if (name == "push") leg.to.zoom = (std::max)(MinZoom, now.zoom * 0.5f);
    else if (name == "pull") leg.to.zoom = (std::min)(MaxZoom, now.zoom * 2.f);
    else if (name == "orbit") { leg.to.yaw = now.yaw + 6.2831853f; leg.ms = ms * 3; leg.ease = Ease::Linear; }
    else if (name == "crash") { leg.to.zoom = (std::max)(MinZoom, now.zoom * 0.35f); leg.ms = 250; leg.ease = Ease::Linear; }
    else if (name == "vertigo") { leg.to.zoom = (std::max)(MinZoom, now.zoom * 0.45f); leg.to.fov = 0.55f; }
    else if (name == "dutch") { leg.to.roll = 0.3f; leg.ms = ms / 2; }
    else return false;
    leg.ms = (std::min)((std::max)(leg.ms, MinLegMs), MaxLegMs);
    legs = {leg};
    return true;
}
inline Control& Phone() { static Control control; return control; }
inline nlohmann::json OffsetsJson(const Offsets& o) {
    return {{"yaw", o.yaw}, {"pitch", o.pitch}, {"zoom", o.zoom}, {"panX", o.panX}, {"panY", o.panY}, {"roll", o.roll}, {"fov", o.fov}};
}
// Numbers only; anything else leaves the field as it was.
inline Offsets ReadOffsets(const nlohmann::json& j, Offsets o) {
    if (!j.is_object()) return o;
    const auto read = [&](const char* name, float& field) { const auto v = j.find(name); if (v != j.end() && v->is_number()) field = v->get<float>(); };
    read("yaw", o.yaw); read("pitch", o.pitch); read("zoom", o.zoom); read("panX", o.panX); read("panY", o.panY); read("roll", o.roll); read("fov", o.fov);
    return Limit(o);
}
inline void Control::SetFile(std::filesystem::path file) {
    std::lock_guard<std::mutex> hold(lock_);
    file_ = std::move(file);
    std::ifstream in(file_);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) return;
    if (j.contains("glideMs") && j["glideMs"].is_number()) settings_.glideMs = (std::min)((std::max)(static_cast<int>(j["glideMs"].get<double>()), MinLegMs), MaxLegMs);
    if (j.contains("views") && j["views"].is_array())
        for (std::size_t i = 0; i < j["views"].size() && i < settings_.views.size(); ++i) {
            const auto& v = j["views"][i];
            if (!v.is_object() || !v.value("used", false)) continue;
            settings_.views[i] = {true, v.value("name", std::string()).substr(0, 40), ReadOffsets(v.value("at", nlohmann::json()), Offsets{})};
        }
}
inline nlohmann::json Control::ViewsJson(const Settings& s) {
    nlohmann::json views = nlohmann::json::array();
    for (const auto& v : s.views) views.push_back({{"used", v.used}, {"name", v.name}, {"at", OffsetsJson(v.at)}});
    return views;
}
inline nlohmann::json Control::Json(const Settings& s) {
    nlohmann::json keys = nlohmann::json::array();
    for (const auto& k : s.keys) keys.push_back({{"frame", k.frame}, {"ease", static_cast<int>(k.ease)}, {"at", OffsetsJson(k.at)}});
    return {{"gliding", s.gliding}, {"glideMs", s.glideMs}, {"views", ViewsJson(s)}, {"live", OffsetsJson(s.live)}, {"free", s.free}, {"dolly", s.dolly}, {"keys", keys}, {"selected", s.selected}, {"loop", s.loop}};
}
// {"saveView":slot,"viewName"} | {"deleteView":slot} | {"goView":slot} | {"tour":true,"loop"} | {"effect":"push|pull|orbit|crash|vertigo|dutch"} | {"glideMs":n} | {"glide":[{"at","ms","hold","ease"}],"loop":bool} | {"stopGlide":true} | {"reset":true} | {"live":{...}} | {"free":bool} | {"dolly":bool} | {"keys":[{"frame","ease","at"}]} | {"addKey":{...}}
inline nlohmann::json Control::Order(const nlohmann::json& body, std::string& error, Clock::time_point now) {
    error.clear();
    if (!body.is_object()) { error = "no order"; return nullptr; }
    std::lock_guard<std::mutex> hold(lock_);
    const auto flag = [&](const char* name, bool& field) { const auto v = body.find(name); if (v != body.end() && v->is_boolean()) field = v->get<bool>(); };
    const auto key = [](const nlohmann::json& j) {
        Key k;
        if (j.is_object() && j.contains("frame") && j["frame"].is_number_integer()) k.frame = (std::min)((std::max)(j["frame"].get<int>(), 0), 1000000);
        if (j.is_object() && j.contains("ease") && j["ease"].is_number_integer()) k.ease = static_cast<Ease>((std::min)((std::max)(j["ease"].get<int>(), 0), 2));
        if (j.is_object() && j.contains("at")) k.at = ReadOffsets(j["at"], Offsets{});
        return k;
    };
    Advance(now);
    const bool takeOver = body.value("reset", false) || body.contains("live") || body.value("stopGlide", false);
    if (takeOver) { legs_.clear(); settings_.gliding = false; }
    if (body.value("reset", false)) { Settings fresh; fresh.views = settings_.views; fresh.glideMs = settings_.glideMs; settings_ = fresh; }
    if (body.contains("live")) settings_.live = ReadOffsets(body["live"], settings_.live);
    flag("free", settings_.free); flag("dolly", settings_.dolly);
    if (body.contains("keys")) {
        if (!body["keys"].is_array() || body["keys"].size() > MaxKeys) { error = "bad keys"; return nullptr; }
        settings_.keys.clear(); settings_.selected = -1;
        for (const auto& j : body["keys"]) settings_.keys.push_back(key(j));
        Tidy(settings_.keys);
    }
    if (body.contains("addKey")) {
        if (settings_.keys.size() >= MaxKeys) { error = "too many keys"; return nullptr; }
        Key added = key(body["addKey"]);
        if (!body["addKey"].contains("frame") && frame_ >= 0) added.frame = frame_;
        settings_.keys.push_back(added);
        Tidy(settings_.keys);
        settings_.selected = -1;
        for (std::size_t i = 0; i < settings_.keys.size(); ++i) if (settings_.keys[i].frame == added.frame) settings_.selected = static_cast<int>(i);
    }
    // The timeline's edits of the keys, by index into the sorted list.
    const auto index = [&](const nlohmann::json& v, int& out) {
        if (!v.is_number_integer() || v.get<int>() < 0 || v.get<int>() >= static_cast<int>(settings_.keys.size())) { error = "bad key"; return false; }
        out = v.get<int>(); return true;
    };
    int at = 0;
    if (body.contains("selectKey")) { if (body["selectKey"].is_null()) settings_.selected = -1; else if (index(body["selectKey"], at)) settings_.selected = at; }
    if (error.empty() && body.contains("delKey") && index(body["delKey"], at)) { settings_.keys.erase(settings_.keys.begin() + at); settings_.selected = -1; }
    if (error.empty() && body.contains("updateKey") && index(body["updateKey"], at)) settings_.keys[at].at = Limit(settings_.live);
    if (error.empty() && body.contains("easeKey") && body["easeKey"].is_object() && index(body["easeKey"].value("key", -1), at)) {
        const int e = body["easeKey"].value("ease", 1);
        settings_.keys[at].ease = static_cast<Ease>((std::min)((std::max)(e, 0), 2));
    }
    if (error.empty() && body.contains("moveKey") && body["moveKey"].is_object() && index(body["moveKey"].value("key", -1), at)) {
        settings_.keys[at].frame = (std::min)((std::max)(body["moveKey"].value("frame", settings_.keys[at].frame), 0), 1000000);
        const int frame = settings_.keys[at].frame;
        Tidy(settings_.keys);
        settings_.selected = -1;
        for (std::size_t i = 0; i < settings_.keys.size(); ++i) if (settings_.keys[i].frame == frame) settings_.selected = static_cast<int>(i);
    }
    flag("loop", settings_.loop);
    if (error.empty() && body.contains("seek") && body["seek"].is_number_integer()) AskSeek(body["seek"].get<int>());
    if (body.contains("glide")) {
        const auto& list = body["glide"];
        if (!list.is_array() || list.empty() || list.size() > MaxLegs) { error = "bad glide"; return nullptr; }
        std::vector<Leg> legs;
        for (const auto& j : list) {
            Leg leg;
            const auto whole = [&](const char* name, int low, int& field) { if (j.is_object() && j.contains(name) && j[name].is_number()) field = (std::min)((std::max)(static_cast<int>(j[name].get<double>()), low), MaxLegMs); };
            whole("ms", MinLegMs, leg.ms); whole("hold", 0, leg.hold);
            if (j.is_object() && j.contains("ease") && j["ease"].is_number_integer()) leg.ease = static_cast<Ease>((std::min)((std::max)(j["ease"].get<int>(), 0), 2));
            if (j.is_object() && j.contains("at")) leg.to = ReadOffsets(j["at"], Offsets{});
            legs.push_back(leg);
        }
        StartGlide(std::move(legs), body.value("loop", false), now);
    }
    const auto slot = [&](const char* name, int& out) {
        const auto v = body.find(name);
        if (v == body.end()) return false;
        if (!v->is_number_integer() || v->get<int>() < 0 || v->get<int>() >= ViewSlots) { error = "bad slot"; return false; }
        out = v->get<int>(); return true;
    };
    int n = 0;
    if (body.contains("glideMs") && body["glideMs"].is_number()) { settings_.glideMs = (std::min)((std::max)(static_cast<int>(body["glideMs"].get<double>()), MinLegMs), MaxLegMs); Save(); }
    if (slot("saveView", n)) {
        std::string name = "View " + std::to_string(n + 1);
        if (body.contains("viewName") && body["viewName"].is_string() && !body["viewName"].get<std::string>().empty()) name = body["viewName"].get<std::string>().substr(0, 40);
        settings_.views[n] = {true, name, settings_.live}; Save();
    }
    if (error.empty() && slot("deleteView", n)) { settings_.views[n] = View{}; Save(); }
    if (error.empty() && slot("goView", n)) {
        if (!settings_.views[n].used) error = "that view is empty";
        else StartGlide({Leg{settings_.views[n].at, settings_.glideMs, 0, Ease::Smooth}}, false, now);
    }
    if (error.empty() && body.value("tour", false)) {
        std::vector<Leg> legs;
        for (const auto& v : settings_.views) if (v.used) legs.push_back(Leg{v.at, settings_.glideMs, 400, Ease::Smooth});
        if (legs.size() < 2) error = "a tour needs two saved views";
        else StartGlide(std::move(legs), body.value("loop", false), now);
    }
    if (error.empty() && body.contains("effect") && body["effect"].is_string()) {
        std::vector<Leg> legs;
        if (!EffectLegs(body["effect"].get<std::string>(), settings_.live, settings_.glideMs, legs)) error = "no such effect";
        else StartGlide(std::move(legs), false, now);
    }
    if (!error.empty()) return nullptr;
    if (body.value("clearKeys", false)) { settings_.keys.clear(); settings_.selected = -1; }
    if (body.value("cancelRender", false)) cancelAsked_ = true;
    if (body.value("render", false)) {
        if (settings_.keys.size() < 2) { error = "a shot needs two keys"; return nullptr; }
        if (render_ != static_cast<int>(Render::Ready)) { error = "watch a replay from Ember's Replays screen first"; return nullptr; }
        settings_.dolly = true; renderAsked_ = true;
    }
    if (settings_.keys.empty()) settings_.dolly = false;
    auto answer = Json(settings_);
    answer["frame"] = frame_.load(); answer["render"] = render_.load(); answer["seek"] = seek_.load(); answer["total"] = total_.load();
    return answer;
}
} } }
