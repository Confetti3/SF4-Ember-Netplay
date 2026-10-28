#pragma once
#include "Speech.hxx"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace sf4e { namespace speech {
// Under Proton (a Steam Deck) Wine has no SAPI voice, so Ember speaks through
// speech_relay.py, which it starts on the host once a session (RelayCommand)
// and talks to through a FIFO. Wine has no Unix sockets and the game runs
// inside Steam's container (pressure-vessel), which shares neither
// speech-dispatcher's socket nor its programs, so the relay is what reaches
// them, and a Piper voice it downloads once. These are the lines Ember writes
// it; speech_relay.py describes what it does with them.

// The name under which the relay keeps a part it has said: the same text at
// the same rate in the same language is the same clip. FNV-1a, 64 bits.
inline std::string ClipKey(const std::string& language, int rate, const std::string& text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : language + '\0' + std::to_string(rate) + '\0' + text) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ull; }
    char key[17];
    std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(hash));
    return key;
}

// A line of tab-separated fields ending in a newline, so tabs and line breaks
// in a part become spaces. Each part follows its key.
inline std::string RelayLine(std::vector<std::string> fields, const std::vector<std::string>& parts, int rate, const std::string& language) {
    for (const auto& part : parts) { fields.push_back(ClipKey(language, rate, part)); fields.push_back(part); }
    std::string line;
    for (auto field : fields) {
        for (auto& c : field) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        line += (line.empty() ? "" : "\t") + field;
    }
    return line + "\n";
}
// Say the parts in order; interrupt drops what Ember is still saying first.
// rate is -10 to 10, volume 0 to 100, language the interface's tag. sound,
// a WAV file's Unix path, plays after the parts (the key "wav" marks it).
inline std::string SayLine(const std::vector<std::string>& parts, bool interrupt, int rate, int volume, const std::string& language, const std::string& sound = {}) {
    auto line = RelayLine({"SAY", interrupt ? "1" : "0", std::to_string(rate), std::to_string(volume), language}, parts, rate, language);
    if (!sound.empty()) line.insert(line.size() - 1, "\twav\t" + sound);
    return line;
}
// Make the parts' clips ahead, at low priority, for a screen that just opened.
inline std::string RenderLine(const std::vector<std::string>& parts, int rate, const std::string& language) {
    return RelayLine({"RENDER", std::to_string(rate), language}, parts, rate, language);
}

// The relay's status file: "system", "downloading <percent>", "natural" or
// "failed", then "pack <state> <bytes>". Anything else, a missing file
// included, is System with no pack.
inline VoiceStatus ParseRelayStatus(const std::string& text) {
    VoiceStatus status;
    const auto end = text.find('\n');
    const std::string first = text.substr(0, end);
    int percent = 0;
    if (first == "natural") status = {Voice::Natural, 100};
    else if (first == "failed") status = {Voice::Failed, 0};
    else if (std::sscanf(first.c_str(), "downloading %d", &percent) == 1 && percent >= 0 && percent <= 100) status = {Voice::Downloading, percent};
    char state[16] = {};
    long long bytes = 0;
    if (end != std::string::npos && std::sscanf(text.c_str() + end + 1, "pack %15s %lld", state, &bytes) == 2 && bytes >= 0) {
        const std::string name = state;
        status.pack = name == "missing" ? Pack::Missing : name == "current" ? Pack::Current : name == "outdated" ? Pack::Outdated :
            name == "removed" ? Pack::Removed : Pack::None;
        status.packBytes = bytes;
    }
    return status;
}
// Ask the relay to download the language's voice pack again, or to remove it.
inline std::string PackLine(bool remove, const std::string& language) {
    return std::string("PACK\t") + (remove ? "remove" : "update") + "\t" + language + "\n";
}

// The Unix command line that starts the relay `script` reading `fifo`.
// Outside a container that is python3 itself. Inside one, the host's busctl,
// run by the host's loader from /run/host, asks the host's systemd over the
// shared session bus (bus is DBUS_SESSION_BUS_ADDRESS) to run it as the
// transient unit `unit`. KillMode=process leaves a speech-dispatcher the
// relay started running, and CollectMode drops the unit once it ends.
inline std::vector<std::string> RelayCommand(bool container, const std::string& bus, const std::string& unit, const std::string& script, const std::string& fifo) {
    if (!container) return {"/usr/bin/python3", script, fifo};
    return {"/run/host/usr/lib/ld-linux-x86-64.so.2", "--library-path", "/run/host/usr/lib:/run/host/usr/lib/systemd",
        "/run/host/usr/bin/busctl", "--address=" + bus, "--", "call", "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager", "StartTransientUnit", "ssa(sv)a(sa(sv))", unit, "replace",
        "3", "KillMode", "s", "process", "CollectMode", "s", "inactive-or-failed", "ExecStart", "a(sasb)", "1",
        "/usr/bin/python3", "3", "python3", script, fifo, "false", "0"};
}
} }
