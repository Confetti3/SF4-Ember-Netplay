#include "TrainingCapture.hxx"
#include <windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>
#include <fstream>
#include <filesystem>
#include <iomanip>

namespace sf4e { namespace training {
namespace { constexpr std::size_t MaximumQueuedRows = 4096; }

TrainingCapture::TrainingCapture() {
    char value[8]{};
    enabled_ = GetEnvironmentVariableA("SF4E_TRAINING_CAPTURE", value, sizeof(value)) > 0 && value[0] == '1';
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
    if (!lock.owns_lock() || rows_.size() >= MaximumQueuedRows) { ++dropped_; return; }
    rows_.push_back({frame, fighters, view.startupFrames, view.startupUnavailable, view.advantage});
    lock.unlock(); wake_.notify_one();
}
void TrainingCapture::Run() {
    PWSTR roaming = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming) != S_OK) return;
    std::wstring directory = std::wstring(roaming) + L"\\sf4e\\logs"; CoTaskMemFree(roaming);
    CreateDirectoryW((directory.substr(0, directory.find_last_of(L'\\'))).c_str(), nullptr);
    CreateDirectoryW(directory.c_str(), nullptr);
    std::filesystem::path outputPath(directory);
    wchar_t processId[16]{};
    _snwprintf_s(processId, _TRUNCATE, L"%lu", GetCurrentProcessId());
    outputPath /= std::wstring(L"training-samples-") + processId + L".csv";
    std::ofstream output(outputPath, std::ios::out | std::ios::trunc);
    if (!output) return;
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
        if (stopping_) break;
    }
}
} }
