#pragma once
#include "ComboFile.hxx"
#include "../common/BoundedRead.hxx"
#include "../platform/DurableFile.hxx"
#include <chrono>
#include <filesystem>
#include <mutex>

// The saved combos: one file each in a folder, so a phone and the PC see the
// same list. Any thread may call it; it holds no game or overlay state.
// ponytail: List reads every file each time (at most MaxCombos); keep an index if the folder ever grows.
namespace sf4e { namespace training {
class ComboStore {
public:
    static constexpr std::size_t MaxCombos = 100;
    void SetDirectory(std::filesystem::path directory) { std::lock_guard<std::mutex> hold(lock_); directory_ = std::move(directory); }
    bool Ready() const { std::lock_guard<std::mutex> hold(lock_); return !directory_.empty(); }
    // Newest first; a file that is not a combo is left out.
    std::vector<ComboSummary> List() {
        std::lock_guard<std::mutex> hold(lock_);
        std::vector<ComboSummary> list;
        for (const auto& id : Ids()) {
            SavedCombo combo;
            if (!Read(id, combo)) continue;
            list.push_back({combo.id, combo.name, combo.fighter, combo.hits, static_cast<int>(combo.frames.size()), combo.created});
        }
        std::sort(list.begin(), list.end(), [](const ComboSummary& a, const ComboSummary& b) { return a.created != b.created ? a.created > b.created : a.id < b.id; });
        return list;
    }
    // Keeps a new combo under a new id (a file brought in never names its own, so it cannot replace one).
    bool Add(SavedCombo combo, std::string& id, std::string& error) {
        std::lock_guard<std::mutex> hold(lock_);
        if (directory_.empty()) { error = "no folder"; return false; }
        std::error_code ignored;
        std::filesystem::create_directories(directory_, ignored);
        if (Ids().size() >= MaxCombos) { error = "too many saved combos"; return false; }
        combo.created = combo.created > 0 ? combo.created : Now();
        for (int attempt = 0; attempt < 8; ++attempt) {
            combo.id = std::to_string(Now()) + "-" + std::to_string(++serial_);
            const auto bytes = EncodeCombo(combo);
            const auto path = File(combo.id);
            const auto written = durable::PublishCreateOnly(path, durable::PartialPath(path), bytes.data(), bytes.size());
            if (written) { id = combo.id; return true; }
        }
        error = "the combo could not be saved"; return false;
    }
    bool Import(const std::string& text, std::string& id, std::string& error) {
        SavedCombo combo;
        return DecodeCombo(text, combo, error) && Add(std::move(combo), id, error);
    }
    bool Load(const std::string& id, SavedCombo& combo) { std::lock_guard<std::mutex> hold(lock_); return ValidComboId(id) && Read(id, combo); }
    // The file as stored, for a download.
    bool Bytes(const std::string& id, std::string& bytes) {
        std::lock_guard<std::mutex> hold(lock_);
        SavedCombo combo;
        if (!ValidComboId(id) || !Read(id, combo, &bytes)) return false;
        return true;
    }
    bool Rename(const std::string& id, const std::string& name, std::string& error) {
        std::lock_guard<std::mutex> hold(lock_);
        SavedCombo combo;
        if (!ValidComboId(id) || !Read(id, combo)) { error = "no such combo"; return false; }
        if (name.empty() || !CleanText(name, ComboNameBytes)) { error = "the name is empty or too long"; return false; }
        combo.name = name;
        const auto bytes = EncodeCombo(combo);
        const auto path = File(id);
        if (!durable::PublishReplace(path, durable::PartialPath(path), bytes.data(), bytes.size())) { error = "the combo could not be saved"; return false; }
        return true;
    }
    bool Delete(const std::string& id) {
        std::lock_guard<std::mutex> hold(lock_);
        std::error_code ignored;
        return ValidComboId(id) && std::filesystem::remove(File(id), ignored);
    }
private:
    static std::int64_t Now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
    std::filesystem::path File(const std::string& id) const { return directory_ / (id + ".json"); }
    std::vector<std::string> Ids() const {
        std::vector<std::string> ids;
        std::error_code ignored;
        for (std::filesystem::directory_iterator at(directory_, ignored), end; !ignored && at != end; at.increment(ignored)) {
            const auto& path = at->path();
            if (path.extension() == ".json" && ValidComboId(path.stem().string())) ids.push_back(path.stem().string());
        }
        return ids;
    }
    // The file must hold its own name as its id, so a copied file is not taken for another.
    bool Read(const std::string& id, SavedCombo& combo, std::string* text = nullptr) const {
        const auto read = durable::ReadBounded(File(id), ComboFileBytes);
        if (read.status != durable::ReadStatus::Read) return false;
        const std::string bytes(read.bytes.begin(), read.bytes.end());
        std::string error;
        if (!DecodeCombo(bytes, combo, error) || combo.id != id) return false;
        if (text) *text = bytes;
        return true;
    }
    mutable std::mutex lock_;
    std::filesystem::path directory_;
    unsigned serial_ = 0;
};
// The one store the phone and the overlay share; the overlay gives it its folder.
inline ComboStore& Combos() { static ComboStore store; return store; }
} }
