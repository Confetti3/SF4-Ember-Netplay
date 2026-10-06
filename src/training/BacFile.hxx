#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "ByteReader.hxx"

// The game's script files (<CODE>.bac), read only as far as a trial needs:
// which hit-data sets a script's hitboxes use, which effect scripts it spawns
// (a fireball's hitbox lives in one) and which scripts it passes into. The
// game's Trial mode names a move by those hit-data ids. Owns only the model
// and its byte layout; it knows nothing of the game or the overlay.
namespace sf4e { namespace bac {
// The largest stock file is under 512 KiB.
constexpr std::size_t MaxBytes = 4 * 1024 * 1024;

// hits: hit-data set of every hitbox that can hit. effects: indices into
// File::effects. next: indices into File::scripts the script passes into by
// itself (on hit, on landing), not those that wait for another input.
struct Script { std::vector<std::int32_t> hits, effects, next; };
// scripts: indexed by action id. An empty slot is an empty Script.
struct File { std::vector<Script> scripts, effects; };
// Adds the ids not already in `to`, keeping order.
inline void Add(std::vector<std::int32_t>& to, std::int32_t id) { if (std::find(to.begin(), to.end(), id) == to.end()) to.push_back(id); }
inline void Add(std::vector<std::int32_t>& to, const std::vector<std::int32_t>& from) { for (const auto id : from) Add(to, id); }

namespace detail {
constexpr std::size_t HeaderSize = 0x28, ScriptHeader = 0x18, ListSize = 12, FlowSize = 8, HitboxSize = 44, EtcSize = 32;
constexpr unsigned Flow = 0, Hitbox = 7, Etc = 10, OnInput = 12;

// One table of scripts. used: bytes read so far. The records of a real file
// never share bytes, so together they fit in the file; this keeps offsets
// that point many scripts at the same bytes from growing the model, or the
// time to read it, past the file's own size.
inline bool Table(const ByteReader& in, std::size_t count, std::uint64_t table, std::vector<Script>& out, std::uint64_t& used, std::string& error) {
    const auto fail = [&](const char* why) { error = why; return false; };
    if (!in.Has(table, count * 4)) return fail("the script table runs past the end of the file");
    out.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t at = in.U32(table + i * 4);
        if (!at) continue;
        if (!in.Has(at, ScriptHeader)) return fail("a script runs past the end of the file");
        const std::size_t lists = in.U16(at + 0x12);
        const std::uint64_t base = at + ScriptHeader;
        if (!in.Has(base, lists * ListSize)) return fail("a script's commands run past the end of the file");
        used += ScriptHeader + lists * ListSize;
        if (used > in.size) return fail("the scripts overlap");
        for (std::size_t j = 0; j < lists; ++j) {
            const std::uint64_t list = base + j * ListSize;
            const unsigned type = in.U16(list);
            const std::size_t size = type == Flow ? FlowSize : type == Hitbox ? HitboxSize : type == Etc ? EtcSize : 0;
            if (!size) continue;
            const std::size_t commands = in.U16(list + 2);
            // The data offset counts from the list's own record.
            const std::uint64_t data = list + in.U32(list + 8);
            if (!in.Has(data, commands * size)) return fail("a script's commands run past the end of the file");
            used += commands * size;
            if (used > in.size) return fail("the scripts overlap");
            for (std::size_t k = 0; k < commands; ++k) {
                const std::uint64_t record = data + k * size;
                if (type == Flow) {
                    // A pass that waits for another input is that input's own move.
                    const auto target = static_cast<std::int16_t>(in.U16(record + 4));
                    if (target >= 0 && in.U16(record) != OnInput) out[i].next.push_back(target);
                } else if (type == Hitbox) {
                    // Type 0 is the box that makes the foe guard; it never hits.
                    const auto id = static_cast<std::int32_t>(in.U32(record + 40));
                    if (in.data[record + 26] && id >= 0) out[i].hits.push_back(id);
                } else if (in.U16(record) == 0 && in.U16(record + 2) == 2) {
                    const auto effect = static_cast<std::int32_t>(in.U32(record + 4));
                    if (effect >= 0) out[i].effects.push_back(effect);
                }
            }
        }
    }
    return true;
}
}

// Reads the scripts and effect scripts of a whole .bac file. Returns false
// with a reason and leaves model alone when a count or offset does not fit.
inline bool Read(const std::uint8_t* bytes, std::size_t size, File& model, std::string& error) {
    using namespace detail;
    const auto fail = [&](const char* why) { error = why; return false; };
    if (!bytes || size < HeaderSize) return fail("the file is shorter than its header");
    if (size > MaxBytes) return fail("the file is too large");
    if (std::memcmp(bytes, "#BAC", 4) != 0) return fail("the file is not a script file");
    const ByteReader in{bytes, size};
    File file;
    std::uint64_t used = 0;
    if (!Table(in, in.U16(0x0C), in.U32(0x14), file.scripts, used, error) ||
        !Table(in, in.U16(0x0E), in.U32(0x18), file.effects, used, error)) return false;
    model = std::move(file);
    return true;
}

// The hit-data ids a script can hit with, each once: its own hitboxes, those
// of the effects it spawns, and those of every script it passes into (a
// throw's hit, a dive's landing).
inline std::vector<std::int32_t> ScriptHits(const File& file, std::int32_t script) {
    std::vector<std::int32_t> hits, todo{script};
    std::vector<char> visited(file.scripts.size());
    while (!todo.empty()) {
        const auto index = todo.back();
        todo.pop_back();
        if (index < 0 || static_cast<std::size_t>(index) >= file.scripts.size() || visited[static_cast<std::size_t>(index)]) continue;
        visited[static_cast<std::size_t>(index)] = 1;
        const Script& now = file.scripts[static_cast<std::size_t>(index)];
        Add(hits, now.hits);
        for (const auto effect : now.effects)
            if (static_cast<std::size_t>(effect) < file.effects.size()) Add(hits, file.effects[static_cast<std::size_t>(effect)].hits);
        todo.insert(todo.end(), now.next.begin(), now.next.end());
    }
    return hits;
}
} }
