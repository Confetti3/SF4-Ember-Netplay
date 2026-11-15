#pragma once
#include "BacFile.hxx"
#include "ByteReader.hxx"
#include "FrameMeter.hxx"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// What the Data panel shows of each fighter, read each frame it is shown
// (TrainingRuntime.cxx: ReadData), and the command file's charge names.
// Knows nothing of the running game or the overlay.
namespace sf4e { namespace training {
// Where a fighter is, as Actor::IsAirborne and IsCrouching tell it; Down is
// lying down or getting up.
enum class Stance { Standing, Crouching, Airborne, Down };
// A charge as the game holds it this frame: its place in the command file,
// the directions to hold (ChargeBack, ChargeDown), the frames to hold them,
// and the frames held so far, which stop at required.
constexpr unsigned ChargeDown = 0x4, ChargeBack = 0x8;
struct Charge { std::size_t index = 0; unsigned directions = 0; int required = 0, held = 0; };
// The most charges a panel reads; Balrog's file has 23 (a turn punch and a turn kick count in ten levels each), El Fuerte's twelve.
constexpr std::size_t MaxCharges = 24;
// A command move's distance rule, as close and far normals pick theirs: at
// least or at most its range from the other fighter, along x or on height.
enum class RangeRule { None, From, Within, HeightFrom, HeightWithin };
// A fighter's files as read once a battle: its scripts, and the names of its
// command file's charges in the game's order. Never changed once read, so
// the readings each frame share them instead of copying.
using SharedScripts = std::shared_ptr<const bac::File>;
using SharedNames = std::shared_ptr<const std::vector<std::string>>;
// The attack that juggles: the hitbox active on this frame, else the next to
// come, else the last (bac::Hit), and its hit data's juggle start; -1 none.
struct Juggle { int start = -1, add = -1, limit = -1; };
struct FighterData {
    bool valid = false;
    // The fighter's files, and the script playing in them and its frame
    // (script frames, which can be fractional).
    SharedScripts scripts; SharedNames chargeNames;
    int script = -1; float scriptFrame = 0;
    // The root position, in the game's units.
    float x = 0, y = 0;
    Stance stance = Stance::Standing; unsigned status = 0;
    // Life and its grey part that refills, the stun gauge, the Super gauge
    // 0 to 1000 (250 a bar) and the Revenge gauge, with their tops.
    int life = 0, lifeMax = 0, recoverable = 0, stun = 0, stunMax = 0, super = 0, superMax = 0, revenge = 0, revengeMax = 0;
    // The juggle count the fighter is under, -1 when not juggled.
    int juggled = -1;
    Juggle juggle;
    RangeRule rangeRule = RangeRule::None; float range = 0;
    std::array<Charge, MaxCharges> charges; std::size_t chargeCount = 0;
    // Juri's Fuhajin stock, bit 0 light, 1 medium, 2 heavy, in place of her button counters; -1 for everyone else.
    int stock = -1;
    // Hakan's oil: frames of it left (0 dry); each Oil Shower adds its own, up to MaxOil. -1 for everyone else.
    int oil = -1;
    // The script in the files, null when they do not have it.
    const bac::Script* Script() const { return scripts && script >= 0 && script < static_cast<int>(scripts->scripts.size()) ? &scripts->scripts[script] : nullptr; }
    // A charge's name in the command file, empty when it has none.
    std::string ChargeName(std::size_t i) const { return chargeNames && i < chargeNames->size() ? (*chargeNames)[i] : std::string(); }
};
// frame: the battle's frame count. distance: between the two roots, along x.
struct DataView { bool valid = false; int frame = 0; float distance = 0; std::array<FighterData, 2> fighters; };

// The hit a fighter's script has at script frame `at`: the one active then,
// else the next, else the last; null when the script has none.
inline const bac::Hit* HitAt(const bac::Script& script, float at) {
    const bac::Hit* next = nullptr;
    for (const auto& hit : script.hits) {
        if (hit.first <= at && at < hit.last) return &hit;
        if (hit.first > at && (!next || hit.first < next->first)) next = &hit;
    }
    return next ? next : script.hits.empty() ? nullptr : &script.hits.back();
}
inline Juggle JuggleOf(const bac::File& file, int script, float at) {
    Juggle juggle;
    if (script < 0 || script >= static_cast<int>(file.scripts.size())) return juggle;
    const bac::Hit* hit = HitAt(file.scripts[script], at);
    if (!hit) return juggle;
    juggle.add = hit->add; juggle.limit = hit->limit;
    if (hit->data >= 0 && hit->data < static_cast<int>(file.juggleStart.size())) juggle.start = file.juggleStart[hit->data];
    return juggle;
}
// A script's frames from 0 to its total as the frame meter colours them
// (ClassifyMeter), in runs of one kind: by its own hits, a gap between two
// recovery; else, with no hit, by the fireball it spawns when that comes out
// before the script ends, as the runtime reads it. None for a script with
// neither. Script frames, not the game frames the meter counts. The cost follows
// the number of hit and spawn boundaries, not the script's length.
struct ScriptRun { int from, to; MeterKind kind; };
inline std::vector<ScriptRun> ScriptRuns(const bac::File& file, int script) {
    std::vector<ScriptRun> runs;
    if (script < 0 || script >= static_cast<int>(file.scripts.size())) return runs;
    const bac::Script& now = file.scripts[script];
    const bool hits = bac::HasTimedHits(now);
    int first = -1, last = -1;
    if (!hits && !(now.hits.empty() && bac::ProjectileBoundary(file, script, first, last) && first < now.total)) return runs;
    // The kind changes only where a hitbox or the spawn begins or ends, so each stretch
    // between two boundaries is read once, at its first frame.
    std::vector<int> cuts{0, now.total};
    if (hits) for (const auto& hit : now.hits) { cuts.push_back(hit.first); cuts.push_back(hit.last); }
    else { cuts.push_back(first); cuts.push_back(last); }
    for (int& cut : cuts) cut = (std::max)(0, (std::min)(now.total, cut));
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        const float at = static_cast<float>(cuts[i]);
        const MeterKind kind = hits ? HitPhaseKind(bac::PhaseAt(now, at)) : BoundaryKind(at, first, last);
        if (runs.empty() || runs.back().kind != kind) runs.push_back({cuts[i], cuts[i + 1], kind});
        else runs.back().to = cuts[i + 1];
    }
    return runs;
}

// A charge name the command file gives that a player can read ("SONICBOOM"); some files name them by code ("CMD_4C"),
// and those are shown by their direction instead.
inline bool ReadableChargeName(const std::string& name) { return !name.empty() && name.rfind("CMD_", 0) != 0; }
// A charge name's words between underscores.
inline std::vector<std::string> NameWords(const std::string& name) {
    std::vector<std::string> out; std::string word;
    for (const char c : name) { if (c == '_') { out.push_back(word); word.clear(); } else word += c; }
    out.push_back(word);
    return out;
}
// The charges of one hold at several levels, as the command file names them
// ("QUESA_L_tameL", "QUESA_M_tameL", "QUESA_H_tameL": one counter, three
// levels), each a list of indices into `charges` in the order of the frames
// they need. Two charges are one counter when they hold the same directions
// for the same frames so far, their names differ in a single word between
// underscores, and they need different frames: the word that changes the
// frames needed is the level, while one that does not is another counter
// ("tameL" against "tameM"). Other charges are a group of one.
inline std::vector<std::vector<std::size_t>> ChargeGroups(const FighterData& fighter) {
    const std::size_t count = fighter.chargeCount;
    const auto words = [&](std::size_t i) { return NameWords(fighter.ChargeName(fighter.charges[i].index)); };
    std::vector<std::size_t> root(count);
    for (std::size_t i = 0; i < count; ++i) root[i] = i;
    const auto find = [&](std::size_t i) { while (root[i] != i) i = root[i] = root[root[i]]; return i; };
    for (std::size_t i = 0; i < count; ++i)
        for (std::size_t j = i + 1; j < count; ++j) {
            const Charge &a = fighter.charges[i], &b = fighter.charges[j];
            if (a.directions != b.directions || a.required == b.required) continue;
            // A level's frames held stop at its own requirement, so of one counter the lower level is
            // either as far as the higher or stopped at its own.
            const Charge &low = a.required < b.required ? a : b, &high = a.required < b.required ? b : a;
            if (high.held != low.held && low.held != low.required) continue;
            if (!ReadableChargeName(fighter.ChargeName(a.index)) || !ReadableChargeName(fighter.ChargeName(b.index))) continue;
            const auto x = words(i), y = words(j);
            if (x.size() != y.size()) continue;
            std::size_t differ = 0;
            for (std::size_t k = 0; k < x.size(); ++k) differ += x[k] != y[k];
            if (differ == 1) root[find(j)] = find(i);
        }
    std::vector<std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < count; ++i) {
        if (find(i) != i) continue;
        std::vector<std::size_t> members;
        for (std::size_t j = i; j < count; ++j) if (find(j) == i) members.push_back(j);
        std::stable_sort(members.begin(), members.end(), [&](std::size_t a, std::size_t b) { return fighter.charges[a].required < fighter.charges[b].required; });
        groups.push_back(std::move(members));
    }
    return groups;
}
// A group's name: its charges' name without the word that tells the levels apart
// ("QUESA_tameL" for QUESA_L_tameL, QUESA_M_tameL, QUESA_H_tameL).
inline std::string ChargeGroupName(const FighterData& fighter, const std::vector<std::size_t>& group) {
    const auto first = NameWords(fighter.ChargeName(fighter.charges[group.front()].index));
    if (group.size() < 2) return fighter.ChargeName(fighter.charges[group.front()].index);
    const auto last = NameWords(fighter.ChargeName(fighter.charges[group.back()].index));
    std::string name;
    for (std::size_t i = 0; i < first.size(); ++i) if (first[i] == last[i]) name += (name.empty() ? "" : "_") + first[i];
    return name;
}

// The most oil Hakan can carry, in frames: the Oil Shower scripts' cap.
constexpr int MaxOil = 3000;
// Juri's command file names three charges (CHARGE_L, CHARGE_M, CHARGE_H) that only count
// a button held, motion or not. Her Fuhajin stock is not one of them: the game keeps it in
// three bits of the fighter (kick light, medium, heavy), set by the motion.
inline bool HasFuhajinStock(const std::vector<std::string>* names) {
    return names && names->size() == 3 && (*names)[0] == "CHARGE_L" && (*names)[1] == "CHARGE_M" && (*names)[2] == "CHARGE_H";
}

// The charge names of a command file (<CODE>.bcm): their count at +0x10 and
// their names from the offsets at +0x1C. The game keeps one state each, in
// this order (Command::CommandImpl::GetChargeStates). Empty when the file is
// not a command file or its table runs off it.
inline std::vector<std::string> ReadChargeNames(const std::uint8_t* bytes, std::size_t size) {
    std::vector<std::string> names;
    if (!bytes || size < 0x20 || std::memcmp(bytes, "#BCM", 4) != 0) return names;
    const ByteReader in{bytes, size};
    const std::size_t count = in.U16(0x10);
    const std::uint64_t table = in.U32(0x1C);
    if (count > 64 || !in.Has(table, count * 4)) return names;
    for (std::size_t i = 0; i < count; ++i) names.push_back(in.Text(in.U32(table + i * 4), 64));
    return names;
}
} }
