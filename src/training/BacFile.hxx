#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "ByteReader.hxx"

// The game's script files (<CODE>.bac), read only as far as the frame meter
// and the Data panel need: which effect scripts it spawns (a fireball's
// hitbox lives in one), when they spawn and whether they can hit, how fast a
// script plays, its name, and its hitboxes' juggle values.
// Owns only the model and its byte layout; it knows nothing of the game or
// the overlay.
namespace sf4e { namespace bac {
// The largest stock file is under 512 KiB.
constexpr std::size_t MaxBytes = 4 * 1024 * 1024;

// A command of list 8: from frame first up to last, the body parts in parts
// (training::OpenParts' bits) are open to what `to` names (2 a strike, 4 a
// throw, 8 a fireball).
// A command of list 4: from script frame `from`, the script moves on `speed`
// of its frames each game frame, until the next one.
struct Speed { int from; float speed; };
// canHit: at least one hitbox can hit. effects: indices into File::effects.
// spawns: one per entry of effects, the frames of the script its spawn
// command covers, {-1, -1} when the file gives none.
// speeds: its list 4. last, interruptible, total: its header's last attack,
// interruptible and last frames, in script frames.
// A hitbox of list 7 that can hit, from script frame first up to last: the
// juggle count up to which it still hits an airborne foe (limit), what it
// adds to that count (add), and its row of the hit data (data).
struct Hit { int first = -1, last = -1; int limit = 0, add = 0; std::int32_t data = -1; };
// A command of list 0 that always goes on in another script of the bank, from script frame at.
struct Flow { int at; std::int32_t script; };
struct Script {
    bool canHit = false; std::vector<std::int32_t> effects; std::vector<std::array<int, 2>> spawns;
    std::vector<Speed> speeds; int last = 0, interruptible = 0, total = 0;
    std::string name; std::vector<Hit> hits; std::vector<Flow> flows;
    // Script frames [first, last) where a move of a cancel list can start
    // (list 6 records whose condition is 1: processCommandCancel starts a
    // pending move only while one is open). Oiled Hakan's dash: from frame 1.
    std::vector<std::array<int, 2>> cancels;
};
// scripts: indexed by action id. An empty slot is an empty Script.
// juggleStart: by hit-data row, the juggle count a launch on a standing foe
// sets (its s16 at +0x2C, Actor::processPendingHit), -1 when unread.
struct File { std::vector<Script> scripts, effects; std::vector<int> juggleStart; };

namespace detail {
constexpr std::size_t HeaderSize = 0x28, ScriptHeader = 0x18, ListSize = 12, FlowSize = 8, HitboxSize = 44, EtcSize = 32;
constexpr std::size_t SpeedSize = 4, CancelSize = 8;
constexpr unsigned Flow = 0, Speeds = 4, Cancels = 6, Hitbox = 7, Etc = 10;

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
        out[i].last = in.U16(at + 2); out[i].interruptible = in.U16(at + 4); out[i].total = in.U16(at + 6);
        const std::size_t lists = in.U16(at + 0x12);
        const std::uint64_t base = at + ScriptHeader;
        if (!in.Has(base, lists * ListSize)) return fail("a script's commands run past the end of the file");
        used += ScriptHeader + lists * ListSize;
        if (used > in.size) return fail("the scripts overlap");
        for (std::size_t j = 0; j < lists; ++j) {
            const std::uint64_t list = base + j * ListSize;
            const unsigned type = in.U16(list);
            const std::size_t size = type == Flow ? FlowSize : type == Speeds ? SpeedSize : type == Cancels ? CancelSize : type == Hitbox ? HitboxSize : type == Etc ? EtcSize : 0;
            if (!size) continue;
            const std::size_t commands = in.U16(list + 2);
            // The data offset counts from the list's own record.
            const std::uint64_t data = list + in.U32(list + 8);
            // So does the frame offset: a first and a last frame a command.
            const std::uint64_t frames = in.U32(list + 4) ? list + in.U32(list + 4) : 0;
            if (!in.Has(data, commands * size)) return fail("a script's commands run past the end of the file");
            used += commands * size;
            if (used > in.size) return fail("the scripts overlap");
            for (std::size_t k = 0; k < commands; ++k) {
                const std::uint64_t record = data + k * size;
                if (type == Flow) {
                    // Kind 0 goes on unconditionally; the meter reads no other.
                    const auto target = static_cast<std::int16_t>(in.U16(record + 4));
                    if (in.U16(record) == 0 && target >= 0 && frames && in.Has(frames + k * 4, 4)) out[i].flows.push_back({static_cast<int>(in.U16(frames + k * 4)), target});
                } else if (type == Hitbox) {
                    // Type 0 is the box that makes the foe guard; it never hits. Type 4 meets
                    // only fireballs (Rose's Soul Reflect, out before its strike).
                    const auto id = static_cast<std::int32_t>(in.U32(record + 40));
                    if (in.data[record + 26] && in.data[record + 26] != 4 && id >= 0) {
                        out[i].canHit = true;
                        const bool timed = frames && in.Has(frames + k * 4, 4);
                        out[i].hits.push_back({timed ? in.U16(frames + k * 4) : -1, timed ? in.U16(frames + k * 4 + 2) : -1,
                            static_cast<std::int8_t>(in.data[record + 33]), static_cast<std::int8_t>(in.data[record + 34]), id});
                    }
                } else if (type == Cancels) {
                    if (in.U32(record) == 1 && frames && in.Has(frames + k * 4, 4)) out[i].cancels.push_back({in.U16(frames + k * 4), in.U16(frames + k * 4 + 2)});
                } else if (type == Speeds) {
                    float speed;
                    std::memcpy(&speed, in.data + record, sizeof speed);
                    if (frames && in.Has(frames + k * 4, 4)) out[i].speeds.push_back({in.U16(frames + k * 4), speed});
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
    // Names: a table of offsets at +0x1C, one a script, to zero-ended text.
    // A name that runs off the file or past 64 bytes is left empty.
    const std::uint64_t names = in.U32(0x1C);
    for (std::size_t i = 0; names && i < file.scripts.size() && in.Has(names + i * 4, 4); ++i) file.scripts[i].name = in.Text(in.U32(names + i * 4), 64);
    // Hit data: a table of offsets at +0x24, its count at +0x10, each row
    // twelve situations of 0x50 bytes; the first is a standing foe's.
    const std::uint64_t rows = in.U32(0x24), count = in.U32(0x10);
    for (std::uint64_t i = 0; rows && i < count && i < 4096 && in.Has(rows + i * 4, 4); ++i) {
        const std::uint64_t row = in.U32(rows + i * 4);
        file.juggleStart.push_back(row && in.Has(row + 0x2C, 2) ? static_cast<std::int16_t>(in.U16(row + 0x2C)) : -1);
    }
    model = std::move(file);
    return true;
}

// Game frames the script takes from script frame `at` to `to`, each moving it
// on by the speed in effect, 1 before any; -1 when the speeds never get there.
// A frame that crosses a speed change plays the rest of it at the new speed,
// not stopping on the change: Cammy getting up, 2 a frame up to 57, read in
// game at 56, 57.5, 58.5.
inline float SpeedAt(const Script& script, float at) {
    const Speed* now = nullptr;
    for (const auto& speed : script.speeds) if (speed.from <= at && (!now || speed.from >= now->from)) now = &speed;
    return now ? now->speed : 1;
}
inline int GameFrames(const Script& script, float at, int to) {
    int frames = 0;
    for (; at < to; ++frames) {
        if (frames >= 4096) return -1;
        for (float left = 1; left > 0;) {
            const float step = SpeedAt(script, at);
            if (!(step > 0)) return -1;
            float next = 1e9f;
            for (const auto& speed : script.speeds) if (speed.from > at && speed.from < next) next = static_cast<float>(speed.from);
            if (at + step * left <= next) { at += step * left; break; }
            left -= (next - at) / step;
            at = next;
        }
    }
    return frames;
}

// Game frames a hit or block reaction takes from script frame `at`, moving
// `speed` a frame, to its interruptible frame. The game spreads the frames up
// to its last over the stun at one speed, the stun less one frame after
// hitstop, then plays on at speed 1 (Actor::setupStunSlide, updateHitStun);
// at speed 1 the two agree. -1 without a speed or a last frame before the end.
// Read off Balrog's reactions to five of Ryu's normals.
inline int StunFrames(const Script& script, float at, float speed) {
    const int end = script.interruptible > 0 ? script.interruptible : script.total;
    if (!(speed > 0) || script.last <= 0 || end <= script.last) return -1;
    const long stretched = (std::max)(0L, std::lround(script.last / speed) - 1 - std::lround(at / speed));
    at += stretched * speed;
    return static_cast<int>(stretched) + (std::max)(0, static_cast<int>(std::ceil(end - at - 0.001f)));
}

// Where a script's own hitboxes stand at script frame `frame`, read after the
// game's update: Untimed when the file does not say (a box without frames),
// NoHit when the script has none. A box from first up to last is out while
// first <= int(frame) < last: the game's clock stops on each whole script frame
// (Engine::advance 0x52F810), so a box is out from the first update that
// reaches its first frame.
enum class HitPhase { Untimed, NoHit, Before, Out, Between, Done };
inline HitPhase PhaseAt(const Script& script, float frame) {
    if (script.hits.empty()) return HitPhase::NoHit;
    const auto untimed = [](const Hit& hit) { return hit.first < 0 || hit.last <= hit.first; };
    if (std::any_of(script.hits.begin(), script.hits.end(), untimed)) return HitPhase::Untimed;
    const int at = static_cast<int>(std::floor(frame));
    bool before = true, after = true;
    for (const auto& hit : script.hits) {
        if (hit.first <= at && at < hit.last) return HitPhase::Out;
        before = before && at < hit.first;
        after = after && at >= hit.last;
    }
    return before ? HitPhase::Before : after ? HitPhase::Done : HitPhase::Between;
}
// Whether the script can be cancelled at script frame `frame`: what a cancel
// out of a dash waits for (training::WaitCancel), a dash having no hit to wait on.
inline bool CancelOpen(const Script& script, float frame) {
    const int at = static_cast<int>(frame);
    return std::any_of(script.cancels.begin(), script.cancels.end(), [at](const std::array<int, 2>& w) { return w[0] <= at && at < w[1]; });
}
// Whether every hitbox of the script is timed and there is at least one.
inline bool HasTimedHits(const Script& script) {
    const HitPhase phase = PhaseAt(script, 0);
    return phase != HitPhase::Untimed && phase != HitPhase::NoHit;
}
// The attack boundary of a script whose attack is an effect it spawns (a
// fireball): the earliest spawn of an effect that can hit, in the terms of the
// script's own header, which counts one frame later than the commands do.
// Read off Ryu's Hadoken: spawn command at 25 to 26, header 26 and 27.
// Not every header agrees (Sagat's Tiger Shot says 24 and 25 for a spawn at
// 24 to 25), so this is for a script whose header says nothing, and may be a
// frame late there.
// False when the script spawns nothing that hits or the file gives no frames.
// delay: game frames from the fireball coming out to its hit, 0 unless its
// effect hits only once it goes on in another (Evil Ryu's falling fireball).
inline bool ProjectileBoundary(const File& file, std::int32_t script, int& first, int& last, int* delay = nullptr) {
    if (script < 0 || static_cast<std::size_t>(script) >= file.scripts.size()) return false;
    const Script& now = file.scripts[static_cast<std::size_t>(script)];
    bool found = false;
    int wait = 0;
    for (std::size_t i = 0; i < now.effects.size(); ++i) {
        const auto effect = static_cast<std::size_t>(now.effects[i]);
        const auto& spawn = now.spawns[i];
        if (effect >= file.effects.size() || spawn[0] < 0 || spawn[1] <= spawn[0]) continue;
        // An effect that hits only once it goes on in another hits the game frames it plays
        // to that later, at its own speed.
        int frames = file.effects[effect].canHit ? 0 : -1;
        for (const auto& flow : file.effects[effect].flows)
            if (frames < 0 && static_cast<std::size_t>(flow.script) < file.effects.size() && file.effects[flow.script].canHit)
                frames = GameFrames(file.effects[effect], 0, flow.at);
        if (frames < 0 || (found && spawn[0] + 1 + frames >= first + wait)) continue;
        first = spawn[0] + 1; last = spawn[1] + 1; wait = frames; found = true;
    }
    if (found && delay) *delay = wait;
    return found;
}
} }
