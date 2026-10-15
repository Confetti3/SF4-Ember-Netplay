#pragma once

// A minute of frames as the game's frame limiter ends them, summed for one
// log line a field report can read: the mean frame rate, the frames that ran
// late (half a period over, so a refresh was missed) and the longest frame.

namespace sf4e { namespace pacing {

struct FrameRateSummary {
    int frames = 0, late = 0;
    double seconds = 0.0, fps = 0.0, lateMs = 0.0, longestMs = 0.0;
};

class FrameRateWindow {
public:
    static constexpr double WindowMs = 60000.0;
    // A frame of frameMs, from the limiter's previous exit to this one, under
    // a period of periodMs (60 fps when the game sets none). True once a
    // window is full; summary then holds it and the next window begins.
    bool Add(double frameMs, double periodMs, FrameRateSummary& summary) {
        const double lateMs = 1.5 * (periodMs > 0.0 ? periodMs : 1000.0 / 60.0);
        ++frames_;
        totalMs_ += frameMs;
        if (frameMs > lateMs) ++late_;
        if (frameMs > longestMs_) longestMs_ = frameMs;
        if (totalMs_ < WindowMs) return false;
        summary.frames = frames_; summary.late = late_;
        summary.seconds = totalMs_ / 1000.0;
        summary.fps = frames_ * 1000.0 / totalMs_;
        summary.lateMs = lateMs; summary.longestMs = longestMs_;
        *this = FrameRateWindow();
        return true;
    }
private:
    int frames_ = 0, late_ = 0;
    double totalMs_ = 0.0, longestMs_ = 0.0;
};

} }
