#include "../training/BacFile.hxx"
#include "game_file_test_support.hxx"
#include "test_support.hxx"
#include <map>

using namespace sf4e;
using Ids = std::vector<std::int32_t>;

// A script file laid out like a stock one: header, both offset tables, then
// each script as its header, its command lists and their records.
struct ScriptRow {
    Ids hits, effects;
    std::vector<std::array<int, 2>> flows; // {type, target}
    Ids guards;                            // hitboxes of type 0, which never hit
    int spawnAt = -1;                      // first frame of every effect spawn; -1 writes no frames
    std::vector<bac::Speed> speeds;        // list 4, written in place of the skipped list
    int interruptible = 0;                 // the header's
};
static Bytes Scripts(const std::map<int, ScriptRow>& scripts, const std::map<int, ScriptRow>& effects) {
    const std::size_t counts[] = {scripts.empty() ? 0 : static_cast<std::size_t>(scripts.rbegin()->first) + 1,
        effects.empty() ? 0 : static_cast<std::size_t>(effects.rbegin()->first) + 1};
    Bytes out(0x28 + 4 * (counts[0] + counts[1]));
    std::memcpy(out.data(), "#BAC", 4);
    std::size_t table = 0x28;
    for (int bank = 0; bank < 2; ++bank) {
        out[0x0C + 2 * bank] = static_cast<std::uint8_t>(counts[bank]);
        out[0x0D + 2 * bank] = static_cast<std::uint8_t>(counts[bank] >> 8);
        Set32(out, 0x14 + 4 * bank, static_cast<std::uint32_t>(table));
        for (const auto& entry : bank ? effects : scripts) {
            const ScriptRow& row = entry.second;
            Set32(out, table + 4 * entry.first, static_cast<std::uint32_t>(out.size()));
            Bytes lists, data;
            const std::size_t listCount = 4;
            const auto list = [&](unsigned type, std::size_t count, int framesFrom = -1) {
                const std::size_t record = lists.size();
                Put16(lists, type); Put16(lists, static_cast<std::uint32_t>(count));
                Put32(lists, framesFrom < 0 ? 0 : static_cast<std::uint32_t>(listCount * 12 - record + data.size()));
                // The other-kind command first, then each spawn a frame later than the one before.
                for (std::size_t i = 0; framesFrom >= 0 && i < count; ++i) { Put16(data, i ? framesFrom + i - 1 : 0); Put16(data, i ? framesFrom + i : 1); }
                Put32(lists, static_cast<std::uint32_t>(listCount * 12 - record + data.size()));
            };
            list(0, row.flows.size());
            for (const auto& flow : row.flows) { Put16(data, flow[0]); Put16(data, 0); Put16(data, flow[1]); Put16(data, 0); }
            list(7, row.hits.size() + row.guards.size());
            for (std::size_t i = 0; i < row.hits.size() + row.guards.size(); ++i) {
                const std::size_t at = data.size();
                data.resize(at + 44);
                data[at + 26] = i < row.hits.size() ? 1 : 0;
                Set32(data, at + 40, static_cast<std::uint32_t>(i < row.hits.size() ? row.hits[i] : row.guards[i - row.hits.size()]));
            }
            list(10, row.effects.size() + 1, row.spawnAt);
            data.resize(data.size() + 32); // An effect command of another kind.
            data[data.size() - 32] = 5;
            for (const auto effect : row.effects) { Put16(data, 0); Put16(data, 2); Put32(data, effect); data.resize(data.size() + 24); }
            if (!row.speeds.empty()) {
                const std::size_t record = lists.size();
                Put16(lists, 4); Put16(lists, static_cast<std::uint32_t>(row.speeds.size()));
                Put32(lists, static_cast<std::uint32_t>(listCount * 12 - record + data.size()));
                for (const auto& speed : row.speeds) { Put16(data, speed.from); Put16(data, speed.from + 1); }
                Put32(lists, static_cast<std::uint32_t>(listCount * 12 - record + data.size()));
                for (const auto& speed : row.speeds) { std::uint32_t bits; std::memcpy(&bits, &speed.speed, 4); Put32(data, bits); }
            } else list(9, 3); // A list the reader skips.
            const std::size_t at = out.size();
            out.resize(at + 0x18);
            out[at + 0x12] = static_cast<std::uint8_t>(listCount);
            out[at + 4] = static_cast<std::uint8_t>(row.interruptible);
            out.insert(out.end(), lists.begin(), lists.end());
            out.insert(out.end(), data.begin(), data.end());
        }
        table += 4 * counts[bank];
    }
    return out;
}
static ScriptRow Script(Ids hits, Ids effects = {}, std::vector<std::array<int, 2>> flows = {}, Ids guards = {}, int spawnAt = -1) {
    return ScriptRow{std::move(hits), std::move(effects), std::move(flows), std::move(guards), spawnAt};
}
int main() {
    std::string error;
    const Bytes bytes = Scripts({{0, Script({}, {1, 0, 0}, {}, {}, 25)}, {1, Script({}, {0})},
        {2, Script({}, {1}, {}, {}, 25)}, {3, Script({10})}, {4, Script({}, {99}, {}, {}, 25)},
        {5, Script({}, {2, 3}, {}, {}, 25)}, {6, Script({}, {}, {{{0, 3}}, {{12, 1}}})}},
        {{0, Script({100})}, {1, Script({}, {}, {}, {100})}, {2, Script({-1})}, {3, Script({})}});
    bac::File model;
    CHECK(ReadExact(bytes, bytes.size(), model, error));
    CHECK(model.scripts.size() == 7 && model.effects.size() == 4);
    CHECK(model.effects[0].canHit && !model.effects[1].canHit && !model.effects[2].canHit);
    CHECK(model.scripts[6].effects.empty() && !model.scripts[6].canHit);
    int first = -1, last = -1;
    CHECK(bac::ProjectileBoundary(model, 0, first, last) && first == 27 && last == 28);
    // List 4 sets how many script frames a game frame plays: Balrog getting up
    // plays 2 a frame up to 50, then 1, so it is free 29 frames on, at 54.
    {
        ScriptRow rise = Script({});
        rise.speeds = {{0, 2.0f}, {50, 1.0f}};
        rise.interruptible = 54;
        const Bytes rising = Scripts({{0, rise}}, {});
        bac::File read;
        CHECK(ReadExact(rising, rising.size(), read, error));
        const bac::Script& script = read.scripts[0];
        CHECK(script.interruptible == 54 && script.speeds.size() == 2 && script.speeds[1].from == 50 && script.speeds[0].speed == 2.0f);
        CHECK(bac::GameFrames(script, 0, 54) == 29 && bac::GameFrames(script, 2, 54) == 28 && bac::GameFrames(script, 54, 54) == 0);
        CHECK(bac::GameFrames(model.scripts[0], 0, 10) == 10 && bac::GameFrames(bac::Script{{}, {}, {}, {{0, 0.0f}}}, 0, 10) == -1);
    }
    // A reaction as the game played Balrog's to Ryu's 5LP, 5MP, 5HP and 2MK,
    // first frame after hitstop, and 5LP's at speed 1 again: frames to free less one.
    {
        const auto reaction = [](int last, int interruptible) { bac::Script script; script.last = last; script.interruptible = interruptible; return script; };
        CHECK(bac::StunFrames(reaction(10, 14), 1.25f, 1.25f) == 12 && bac::StunFrames(reaction(12, 16), 0.6f, 0.6f) == 23);
        CHECK(bac::StunFrames(reaction(39, 43), 2.4375f, 2.4375f) == 21 && bac::StunFrames(reaction(12, 16), 1, 1) == 15);
        CHECK(bac::StunFrames(reaction(10, 14), 9.75f, 1) == 5 && bac::StunFrames(reaction(10, 14), 1, 0) == -1 && bac::StunFrames(reaction(0, 14), 1, 1) == -1);
    }
    for (int action : {-1, 1, 2, 3, 4, 5, 6, 999}) {
        first = last = -1;
        CHECK(!bac::ProjectileBoundary(model, action, first, last) && first == -1 && last == -1);
    }
    // Exact-size reads retain the old truncation and mutation coverage.
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        bac::File truncated;
        if (!ReadExact(bytes, size, truncated, error)) CHECK(truncated.scripts.empty() && truncated.effects.empty());
    }
    const auto refused = [&](const Bytes& bad) {
        bac::File unchanged = model;
        CHECK(!ReadExact(bad, bad.size(), unchanged, error) && !error.empty());
        CHECK(unchanged.scripts.size() == 7 && unchanged.effects.size() == 4 && unchanged.effects[0].canHit);
        int a = -1, b = -1;
        CHECK(bac::ProjectileBoundary(unchanged, 0, a, b) && a == 27 && b == 28);
    };
    CHECK(!bac::Read(nullptr, 64, model, error));
    Bytes bad = bytes; bad[1] = 'X'; refused(bad);
    bad = bytes; Set32(bad, 0x14, 0xffffffff); refused(bad);
    bad = bytes; bad[0x0C] = bad[0x0D] = 0xff; refused(bad);
    bad = bytes; Set32(bad, 0x28, 0xfffffff0); refused(bad);
    const sf4e::ByteReader reader{bytes.data(), bytes.size()};
    const std::size_t script = reader.U32(0x28 + 6 * 4), flow = script + 0x18;
    // Flow commands no longer populate the model, but malformed flow data
    // and the overlap budget still refuse the file.
    bad = bytes; Set32(bad, flow + 8, 0xffffffff); refused(bad);
    bad = bytes; bad[flow + 2] = bad[flow + 3] = 0xff; refused(bad);
    Bytes same(0x28 + 4 * 2000 + 0x18 + 12 + 8);
    std::memcpy(same.data(), "#BAC", 4);
    same[0x0C] = 2000 & 0xFF; same[0x0D] = 2000 >> 8;
    Set32(same, 0x14, 0x28);
    const std::size_t one = 0x28 + 4 * 2000;
    for (int i = 0; i < 2000; ++i) Set32(same, 0x28 + 4 * i, static_cast<std::uint32_t>(one));
    same[one + 0x12] = 1; same[one + 0x1A] = 1; same[one + 0x20] = 12;
    refused(same); CHECK(error == "the scripts overlap");
    Bytes huge(bac::MaxBytes + 1); refused(huge);
    for (std::size_t at = 0; at < bytes.size(); ++at) for (const std::uint8_t value : {std::uint8_t(0x7F), std::uint8_t(0xFF)}) {
        bad = bytes; bad[at] = value;
        bac::File mutated;
        if (ReadExact(bad, bad.size(), mutated, error)) for (std::int32_t action = -1; action < 10; ++action) {
            int a = -1, b = -1;
            bac::ProjectileBoundary(mutated, action, a, b);
        } else CHECK(mutated.scripts.empty() && mutated.effects.empty());
    }
    // Missing or incomplete optional spawn frames keep the effect but give
    // no usable boundary, rather than reading outside the supplied bytes.
    bad = bytes;
    const std::size_t spawnList = reader.U32(0x28) + 0x18 + 2 * 12;
    Set32(bad, spawnList + 4, 0xffffffff);
    bac::File untimed;
    CHECK(ReadExact(bad, bad.size(), untimed, error));
    first = last = -1;
    CHECK(!bac::ProjectileBoundary(untimed, 0, first, last));
    return 0;
}
