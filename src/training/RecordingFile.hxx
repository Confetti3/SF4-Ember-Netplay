#pragma once
#include "TrainingSession.hxx"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// A recording slot as a file, so a dummy recording can be kept by name and
// loaded again: {"format":"sf4e-recording","version":2,"frames":[[mapped,raw,wait,offset],...]}.
// Version 2 holds the frames as a slot does, for a fighter facing right.
// Version 1 held the pad's own directions and not the facing; it was
// recorded on the dummy, which starts on the right facing left, so that
// facing is assumed and its frames are turned on import.
namespace sf4e { namespace training {
constexpr const char* RecordingFormat = "sf4e-recording";
constexpr int RecordingVersion = 2;
inline std::string ExportRecording(const std::vector<Input>& frames) {
    nlohmann::json value = nlohmann::json::object();
    value["format"] = RecordingFormat; value["version"] = RecordingVersion;
    value["frames"] = nlohmann::json::array();
    for (const auto& frame : frames) value["frames"].push_back({frame.mapped, frame.raw, frame.wait, frame.offset});
    return value.dump();
}
inline bool ImportRecording(const std::string& text, std::vector<Input>& frames, std::string& error) {
    frames.clear();
    const auto value = nlohmann::json::parse(text, nullptr, false);
    if (!value.is_object()) { error = "this is not a recording"; return false; }
    const auto format = value.find("format");
    if (format == value.end() || !format->is_string() || format->get<std::string>() != RecordingFormat) { error = "this is not a recording"; return false; }
    // A file from before versions were read is a version 1 file.
    const auto version = value.value("version", nlohmann::json(1u));
    if (!version.is_number_unsigned() || version < 1) { error = "this is not a recording"; return false; }
    if (version > RecordingVersion) { error = "the recording is from a newer version"; return false; }
    const bool padDirections = version == 1;
    const auto list = value.find("frames");
    if (list == value.end() || !list->is_array() || list->empty()) { error = "the recording has no frames"; return false; }
    if (list->size() > static_cast<std::size_t>(MaxFrames)) { error = "the recording is too long"; return false; }
    for (const auto& frame : *list) {
        if (!frame.is_array() || frame.size() != 4 || !frame[0].is_number_unsigned() || !frame[1].is_number_unsigned() ||
            !frame[2].is_number_unsigned() || !frame[3].is_number_integer()) { error = "a frame is not four numbers"; frames.clear(); return false; }
        frames.push_back({frame[0].get<unsigned>(), frame[1].get<unsigned>(), static_cast<unsigned char>(frame[2].get<unsigned>() & 3),
            static_cast<signed char>((std::max)(MinOffset, (std::min)(MaxOffset, frame[3].get<int>())))});
        if (padDirections) frames.back() = Mirror(frames.back());
    }
    return true;
}
} }
