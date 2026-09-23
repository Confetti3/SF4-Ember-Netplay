#include "TrainingCapture.hxx"
#include "../common/EnvFlag.hxx"
#include <windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <spdlog/spdlog.h>

namespace sf4e { namespace training {
namespace { constexpr std::size_t MaximumQueuedRows = 4096; }

TrainingCapture::TrainingCapture() {
    enabled_ = EnvFlag("SF4E_TRAINING_CAPTURE");
    if (enabled_) worker_ = std::thread([this] { Run(); });
}
TrainingCapture::~TrainingCapture() {
    if (!enabled_) return;
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    wake_.notify_one();
    if (worker_.joinable()) worker_.join();
}
void TrainingCapture::Record(int frame, const std::array<FighterSample, 2>& fighters, const MeterView& view) {
    if (!enabled_) return;
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || failed_ || rows_.size() >= MaximumQueuedRows) { ++dropped_; return; }
    rows_.push_back({frame, fighters, view.startupFrames, view.startupUnavailable, view.advantage});
    lock.unlock(); wake_.notify_one();
}
// A capture that cannot be written says so once and counts every row it
// loses, instead of leaving a missing or short CSV (ledger H-014).
void TrainingCapture::Fail(const char* what) {
    spdlog::error("Training capture: {}; samples are not being recorded", what);
    std::lock_guard<std::mutex> lock(mutex_);
    failed_ = true;
    dropped_ += rows_.size();
    rows_.clear();
}
void TrainingCapture::Run() {
    PWSTR roaming = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming) != S_OK) {
        CoTaskMemFree(roaming);
        return Fail("the AppData folder is unavailable");
    }
    std::wstring directory = std::wstring(roaming) + L"\\sf4e\\logs"; CoTaskMemFree(roaming);
    CreateDirectoryW((directory.substr(0, directory.find_last_of(L'\\'))).c_str(), nullptr);
    CreateDirectoryW(directory.c_str(), nullptr);
    std::filesystem::path outputPath(directory);
    wchar_t processId[16]{};
    _snwprintf_s(processId, _TRUNCATE, L"%lu", GetCurrentProcessId());
    outputPath /= std::wstring(L"training-samples-") + processId + L".csv";
    std::ofstream output(outputPath, std::ios::out | std::ios::trunc);
    if (!output) return Fail("the CSV file could not be opened");
    output << "frame,side,status,action,action_frame,posture,time_scale,inhibited,attack_start,attack_end,boundary_source,startup,startup_reason,advantage,advantage_valid,advantage_pending,advantage_reason,valid,damage,combo_damage,health,dropped\n";
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [&] { return stopping_ || !rows_.empty(); });
        while (!rows_.empty()) {
            Row row = std::move(rows_.front()); rows_.pop_front(); lock.unlock();
            for (int side = 0; side < 2; ++side) {
                const auto& f = row.fighters[side];
                output << row.frame << ',' << side << ',' << f.status << ',' << f.action << ','
                    << std::setprecision(9) << f.actionFrame << ',' << f.posture << ',' << f.timeScale << ','
                    << (f.basicActionInhibited ? 1 : 0) << ',' << f.firstActiveFrame << ',' << f.lastActiveFrame << ','
                    << static_cast<unsigned>(f.boundaryProvenance) << ',' << row.startup[side] << ','
                    << static_cast<unsigned>(row.startupUnavailable[side]) << ',' << row.advantage.frames[side] << ','
                    << (row.advantage.valid ? 1 : 0) << ',' << (row.advantage.pending ? 1 : 0) << ','
                    << static_cast<unsigned>(row.advantage.unavailable) << ',' << (f.valid ? 1 : 0) << ','
                    << f.damage << ',' << f.comboDamage << ',' << f.health << ',' << dropped_.load() << '\n';
            }
            lock.lock();
        }
        output.flush();
        if (!output) {
            lock.unlock();
            return Fail("writing the CSV file failed");
        }
        if (stopping_) break;
    }
}
} }
