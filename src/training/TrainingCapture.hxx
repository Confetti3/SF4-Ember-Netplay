#pragma once
#include "FrameMeter.hxx"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace sf4e { namespace training {
class TrainingCapture {
public:
    TrainingCapture();
    ~TrainingCapture();
    bool Enabled() const { return enabled_; }
    void Record(int frame, const std::array<FighterSample, 2>& fighters, const MeterView& view);
    std::uint64_t Dropped() const { return dropped_.load(); }
private:
    struct Row {
        int frame;
        std::array<FighterSample, 2> fighters;
        std::array<int, 2> startup;
        std::array<MeasurementUnavailable, 2> startupUnavailable;
        FrameAdvantage advantage;
    };
    void Run();
    bool enabled_ = false, stopping_ = false;
    std::atomic<std::uint64_t> dropped_{0};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Row> rows_;
    std::thread worker_;
};
} }
