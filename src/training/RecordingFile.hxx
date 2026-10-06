#pragma once
#include "TrainingSession.hxx"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// A recording slot as a file, so a dummy recording can be kept by name and
// loaded again: {"format":"sf4e-recording","frames":[[mapped,raw,wait,offset],...]}.
namespace sf4e { namespace training {
constexpr const char* RecordingFormat = "sf4e-recording";
inline std::string ExportRecording(const std::vector<Input>& frames) {
    nlohmann::json value = nlohmann::json::object();
    value["format"] = RecordingFormat; value["version"] = 1;
    value["frames"] = nlohmann::json::array();
    for (const auto& frame : frames) value["frames"].push_back({frame.mapped, frame.raw, frame.wait, frame.offset});
    return value.dump();
}
inline bool ImportRecording(const std::string& text, std::vector<Input>& frames, std::string& error) {
    frames.clear();
    const auto value = nlohmann::json::parse(text, nullptr, false);
    if (!value.is_object() || value.value("format", "") != RecordingFormat) { error = "this is not a recording"; return false; }
    const auto list = value.find("frames");
    if (list == value.end() || !list->is_array() || list->empty()) { error = "the recording has no frames"; return false; }
    if (list->size() > static_cast<std::size_t>(MaxFrames)) { error = "the recording is too long"; return false; }
    for (const auto& frame : *list) {
        if (!frame.is_array() || frame.size() != 4 || !frame[0].is_number_unsigned() || !frame[1].is_number_unsigned() ||
            !frame[2].is_number_unsigned() || !frame[3].is_number_unsigned()) { error = "a frame is not four numbers"; frames.clear(); return false; }
        frames.push_back({frame[0].get<unsigned>(), frame[1].get<unsigned>(), static_cast<unsigned char>(frame[2].get<unsigned>() & 3), static_cast<unsigned char>(frame[3].get<unsigned>() & 0xff)});
    }
    return true;
}
} }
