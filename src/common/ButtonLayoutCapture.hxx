#pragma once
#include "ButtonLayoutTypes.hxx"
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace sf4e { namespace gamepads {
enum class LayoutCancel { None, Start, Direction, Escape, Back, Timeout, State };
inline const char* CancelName(LayoutCancel reason) {
    switch (reason) {
    case LayoutCancel::Start: return "start";
    case LayoutCancel::Direction: return "direction";
    case LayoutCancel::Escape: return "escape";
    case LayoutCancel::Back: return "back";
    case LayoutCancel::Timeout: return "timeout";
    case LayoutCancel::State: return "state";
    default: return "";
    }
}
struct LayoutPress { std::optional<unsigned> button; LayoutCancel cancelled = LayoutCancel::None; };
class LayoutCapture {
public:
    // Seconds with no press before a capture cancels itself.
    static constexpr double Timeout = 10;
    void Begin(double now, const std::vector<std::string>& held) { active_ = true; armed_ = held.empty(); began_ = now; }
    bool Active() const { return active_; }
    // The part of the timeout still to run, 1 at Begin and 0 once it is spent or the capture ended.
    double Left(double now) const { return active_ ? (std::max)(0.0, 1 - (now - began_) / Timeout) : 0; }
    LayoutPress Read(double now, const std::vector<std::string>& held, const ListedButtons& listed,
        LayoutCancel cancel = LayoutCancel::None) {
        if (!active_) return {};
        if (cancel == LayoutCancel::None && now - began_ >= Timeout) cancel = LayoutCancel::Timeout;
        if (cancel != LayoutCancel::None) { active_ = false; return {{}, cancel}; }
        if (!armed_) { armed_ = held.empty(); return {}; }
        const auto down = [&](const char* id) { return std::find(held.begin(), held.end(), id) != held.end(); };
        if (down("Start")) return Read(now, held, listed, LayoutCancel::Start);
        for (const auto* id : {"up", "down", "left", "right"}) if (down(id)) return Read(now, held, listed, LayoutCancel::Direction);
        for (unsigned i = 0; i < LayoutGlyphs.size(); ++i) if (listed[i] && down(LayoutGlyphs[i])) {
            active_ = false; return {i, LayoutCancel::None};
        }
        return {};
    }
private:
    bool active_ = false, armed_ = false;
    double began_ = 0;
};
} }
