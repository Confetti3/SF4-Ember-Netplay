#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "ByteReader.hxx"

// The game's script files (<CODE>.bac), read only as far as the frame meter
// needs: which effect scripts it spawns (a fireball's hitbox lives in one),
// when they spawn and whether they can hit.
// Owns only the model and its byte layout; it knows nothing of the game or
// the overlay.
namespace sf4e { namespace bac {
// The largest stock file is under 512 KiB.
constexpr std::size_t MaxBytes = 4 * 1024 * 1024;

// canHit: at least one hitbox can hit. effects: indices into File::effects.
// spawns: one per entry of effects, the frames of the script its spawn
// command covers, {-1, -1} when the file gives none.
struct Script { bool canHit = false; std::vector<std::int32_t> effects; std::vector<std::array<int, 2>> spawns; };
// scripts: indexed by action id. An empty slot is an empty Script.
struct File { std::vector<Script> scripts, effects; };

namespace detail {
constexpr std::size_t HeaderSize = 0x28, ScriptHeader = 0x18, ListSize = 12, FlowSize = 8, HitboxSize = 44, EtcSize = 32;
constexpr unsigned Flow = 0, Hitbox = 7, Etc = 10;

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
            // So does the frame offset: a first and a last frame a command.
            const std::uint64_t frames = in.U32(list + 4) ? list + in.U32(list + 4) : 0;
            if (!in.Has(data, commands * size)) return fail("a script's commands run past the end of the file");
            used += commands * size;
            if (used > in.size) return fail("the scripts overlap");
            // Flow records are unused by the meter, but still take part in
            // the command bounds and overlap budget above.
            if (type == Flow) continue;
            for (std::size_t k = 0; k < commands; ++k) {
                const std::uint64_t record = data + k * size;
                if (type == Hitbox) {
                    // Type 0 is the box that makes the foe guard; it never hits.
                    const auto id = static_cast<std::int32_t>(in.U32(record + 40));
                    if (in.data[record + 26] && id >= 0) out[i].canHit = true;
                } else if (in.U16(record) == 0 && in.U16(record + 2) == 2) {
                    const auto effect = static_cast<std::int32_t>(in.U32(record + 4));
                    if (effect < 0) continue;
                    out[i].effects.push_back(effect);
                    const bool timed = frames && in.Has(frames + k * 4, 4);
                    out[i].spawns.push_back({timed ? in.U16(frames + k * 4) : -1, timed ? in.U16(frames + k * 4 + 2) : -1});
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

// The attack boundary of a script whose attack is an effect it spawns (a
// fireball): the earliest spawn of an effect that can hit, in the terms of the
// script's own header, which counts one frame later than the commands do.
// Read off Ryu's Hadoken: spawn command at 25 to 26, header 26 and 27.
// Not every header agrees (Sagat's Tiger Shot says 24 and 25 for a spawn at
// 24 to 25), so this is for a script whose header says nothing, and may be a
// frame late there.
// False when the script spawns nothing that hits or the file gives no frames.
inline bool ProjectileBoundary(const File& file, std::int32_t script, int& first, int& last) {
    if (script < 0 || static_cast<std::size_t>(script) >= file.scripts.size()) return false;
    const Script& now = file.scripts[static_cast<std::size_t>(script)];
    bool found = false;
    for (std::size_t i = 0; i < now.effects.size(); ++i) {
        const auto effect = static_cast<std::size_t>(now.effects[i]);
        const auto& spawn = now.spawns[i];
        if (effect >= file.effects.size() || !file.effects[effect].canHit || spawn[0] < 0 || spawn[1] <= spawn[0]) continue;
        if (found && spawn[0] + 1 >= first) continue;
        first = spawn[0] + 1; last = spawn[1] + 1; found = true;
    }
    return found;
}
} }
