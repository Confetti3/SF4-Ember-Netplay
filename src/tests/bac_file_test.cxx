#include "../training/BacFile.hxx"
#include "../training/FighterData.hxx"
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
    Ids reflects;                          // hitboxes of type 4, which meet only fireballs
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
            list(0, row.flows.size(), row.flows.empty() ? -1 : 25);
            for (const auto& flow : row.flows) { Put16(data, flow[0]); Put16(data, 0); Put16(data, flow[1]); Put16(data, 0); }
            list(7, row.hits.size() + row.guards.size() + row.reflects.size());
            for (std::size_t i = 0; i < row.hits.size() + row.guards.size() + row.reflects.size(); ++i) {
                const std::size_t at = data.size();
                data.resize(at + 44);
                data[at + 26] = i < row.hits.size() ? 1 : i < row.hits.size() + row.guards.size() ? 0 : 4;
                // A juggle limit and add a box (Hit::limit, Hit::add): 7 and 2, a guard's 0.
                data[at + 33] = i < row.hits.size() ? 7 : 0; data[at + 34] = i < row.hits.size() ? 2 : 0;
                Set32(data, at + 40, static_cast<std::uint32_t>(i < row.hits.size() ? row.hits[i] :
                    i < row.hits.size() + row.guards.size() ? row.guards[i - row.hits.size()] : row.reflects[i - row.hits.size() - row.guards.size()]));
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
    {
        ScriptRow reflect = Script({});
        reflect.reflects = {124};
        const Bytes reflecting = Scripts({{0, reflect}}, {});
        bac::File read;
        CHECK(ReadExact(reflecting, reflecting.size(), read, error) && read.scripts[0].hits.empty() && !read.scripts[0].canHit);
    }
    // Of its two jumps only kind 0 is kept, with its first frame.
    CHECK(model.scripts[6].flows.size() == 1 && model.scripts[6].flows[0].at == 0 && model.scripts[6].flows[0].script == 3);
    // A hitbox that can hit keeps its juggle values and hit-data row; a guard box is no hit.
    CHECK(model.scripts[3].hits.size() == 1 && model.scripts[3].hits[0].limit == 7 &&
        model.scripts[3].hits[0].add == 2 && model.scripts[3].hits[0].data == 10 && model.effects[1].hits.empty());
    // With no name or hit-data table (offset 0) nothing is named and no juggle start is read.
    CHECK(model.scripts[3].name.empty() && model.juggleStart.empty());
    {
        // Names at +0x1C, one a script, and two hit-data rows at +0x24 (count
        // at +0x10), the second's juggle start 3; a name past the end is left empty.
        Bytes named = bytes;
        const std::size_t table = named.size();
        named.resize(table + 4 * 7);
        const std::size_t text = named.size();
        const char words[] = "STAND\0CROUCH\0";
        named.insert(named.end(), words, words + sizeof words - 1);
        Set32(named, table, static_cast<std::uint32_t>(text)); Set32(named, table + 4, static_cast<std::uint32_t>(text + 6));
        Set32(named, table + 8, 0xFFFFFFF0u);
        Set32(named, 0x1C, static_cast<std::uint32_t>(table));
        const std::size_t rows = named.size();
        named.resize(rows + 8 + 2 * 12 * 0x50);
        Set32(named, rows, static_cast<std::uint32_t>(rows + 8)); Set32(named, rows + 4, static_cast<std::uint32_t>(rows + 8 + 12 * 0x50));
        named[rows + 8 + 12 * 0x50 + 0x2C] = 3;
        Set32(named, 0x24, static_cast<std::uint32_t>(rows)); Set32(named, 0x10, 2);
        bac::File read;
        CHECK(ReadExact(named, named.size(), read, error));
        CHECK(read.scripts[0].name == "STAND" && read.scripts[1].name == "CROUCH" && read.scripts[2].name.empty());
        CHECK(read.juggleStart.size() == 2 && read.juggleStart[0] == 0 && read.juggleStart[1] == 3);
    }
    // The Data panel's juggle values: the hit active at a frame, else the next, else the last.
    {
        bac::File file; file.juggleStart = {0, 3};
        bac::Script script;
        script.hits = {{5, 7, 0, 1, 0}, {9, 11, 7, 2, 1}};
        file.scripts = {script, bac::Script{}};
        CHECK(training::HitAt(script, 2)->first == 5 && training::HitAt(script, 5)->first == 5 && training::HitAt(script, 7)->first == 9);
        CHECK(training::HitAt(script, 20)->first == 9 && !training::HitAt(bac::Script{}, 0));
        const auto juggle = training::JuggleOf(file, 0, 10);
        CHECK(juggle.start == 3 && juggle.add == 2 && juggle.limit == 7);
        CHECK(training::JuggleOf(file, 1, 0).limit == -1 && training::JuggleOf(file, 5, 0).start == -1 && training::JuggleOf(file, -1, 0).add == -1);
    }
    // The Data panel's timeline colours a script as the frame meter does: the gap between two
    // hits as recovery, and a fireball's script from its spawn; a script with neither, none.
    {
        using training::MeterKind;
        bac::File file;
        file.scripts.resize(3); file.effects.resize(1);
        file.scripts[0].total = 14; file.scripts[0].hits = {{5, 7, 0, 1, 0}, {9, 11, 7, 2, 1}};
        file.scripts[1].total = 30; file.scripts[1].effects = {0}; file.scripts[1].spawns = {{{12, 13}}};
        file.effects[0].canHit = true;
        file.scripts[2].total = 20;
        const auto runs = [&](int script) {
            std::vector<std::array<int, 3>> out;
            for (const auto& run : training::ScriptRuns(file, script)) out.push_back({{run.from, run.to, static_cast<int>(run.kind)}});
            return out;
        };
        const int startup = static_cast<int>(MeterKind::Startup), active = static_cast<int>(MeterKind::Active), recovery = static_cast<int>(MeterKind::Recovery);
        CHECK((runs(0) == std::vector<std::array<int, 3>>{{{0, 5, startup}}, {{5, 7, active}}, {{7, 9, recovery}}, {{9, 11, active}}, {{11, 14, recovery}}}));
        CHECK((runs(1) == std::vector<std::array<int, 3>>{{{0, 13, startup}}, {{13, 14, active}}, {{14, 30, recovery}}}));
        CHECK(runs(2).empty() && runs(7).empty());
        // A spawn the script ends before is no attack of its own, as the runtime reads it.
        file.scripts[1].total = 13;
        CHECK(runs(1).empty());
    }
    // Runs of a long script with overlapping, out-of-order and clipped hit intervals match the
    // classification of every frame, projectile scripts included.
    {
        using training::MeterKind;
        bac::File file;
        file.scripts.resize(2); file.effects.resize(1); file.effects[0].canHit = true;
        file.scripts[0].total = 65535;
        file.scripts[0].hits = {{40000, 40010, 0, 1, 0}, {5, 30, 0, 1, 0}, {20, 50, 0, 1, 0}, {70000, 70005, 0, 1, 0}, {65534, 65535, 0, 1, 0}};
        file.scripts[1].total = 65535; file.scripts[1].effects = {0}; file.scripts[1].spawns = {{{30000, 30001}}};
        for (int script = 0; script < 2; ++script) {
            const auto& now = file.scripts[script];
            int first = -1, last = -1;
            if (script) bac::ProjectileBoundary(file, script, first, last);
            std::vector<training::ScriptRun> expect;
            for (int frame = 0; frame < now.total; ++frame) {
                const float at = static_cast<float>(frame);
                const MeterKind kind = script ? training::BoundaryKind(at, first, last) : training::HitPhaseKind(bac::PhaseAt(now, at));
                if (expect.empty() || expect.back().kind != kind) expect.push_back({frame, frame + 1, kind});
                else expect.back().to = frame + 1;
            }
            const auto runs = training::ScriptRuns(file, script);
            bool same = runs.size() == expect.size();
            for (std::size_t i = 0; same && i < runs.size(); ++i)
                same = runs[i].from == expect[i].from && runs[i].to == expect[i].to && runs[i].kind == expect[i].kind;
            CHECK(same && runs.size() > 2);
        }
    }
    // A command file's charge names, as Guile's.
    {
        Bytes bcm(0x20);
        std::memcpy(bcm.data(), "#BCM", 4);
        bcm[0x10] = 2;
        const std::size_t names = bcm.size();
        bcm.resize(names + 8);
        const std::size_t text = bcm.size();
        const char words[] = "SONICBOOM\0SOMERSAULT\0";
        bcm.insert(bcm.end(), words, words + sizeof words - 1);
        Set32(bcm, names, static_cast<std::uint32_t>(text)); Set32(bcm, names + 4, static_cast<std::uint32_t>(text + 10));
        Set32(bcm, 0x1C, static_cast<std::uint32_t>(names));
        const auto charges = training::ReadChargeNames(bcm.data(), bcm.size());
        CHECK(charges.size() == 2 && charges[0] == "SONICBOOM" && charges[1] == "SOMERSAULT");
        // Cut short: no table, or a name that runs off the file is left empty.
        for (std::size_t size = 0; size < bcm.size(); ++size) {
            const auto cut = training::ReadChargeNames(bcm.data(), size);
            CHECK(cut.empty() || (cut.size() == 2 && (cut[1].empty() || cut[1] == "SOMERSAULT")));
        }
        Bytes other = bcm; other[1] = 'X';
        CHECK(training::ReadChargeNames(other.data(), other.size()).empty() && training::ReadChargeNames(nullptr, 64).empty());
    }
    int first = -1, last = -1, delay = -1;
    CHECK(bac::ProjectileBoundary(model, 0, first, last, &delay) && first == 27 && last == 28);
    // Evil Ryu's falling fireball (421P): the effect spawned at 47 hits once it goes on in another at its
    // frame 25, 17 game frames on at 1.5 a frame.
    {
        bac::File fall;
        fall.scripts.resize(1); fall.effects.resize(2);
        fall.scripts[0].effects = {0}; fall.scripts[0].spawns = {{{47, 48}}};
        fall.effects[0].speeds = {{0, 1.5f}}; fall.effects[0].flows = {{25, 1}};
        fall.effects[1].canHit = true;
        int a = -1, b = -1;
        CHECK(bac::ProjectileBoundary(fall, 0, a, b, &delay) && a == 48 && b == 49 && delay == 17);
        fall.effects[0].flows = {{25, 0}};
        CHECK(!bac::ProjectileBoundary(fall, 0, a, b, &delay));
    }
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
        // Cammy's plays 2 a frame up to 57: the frame from 56 ends at 57.5,
        // so 58 is 30 frames on, not 29 (in game).
        bac::Script cammy;
        cammy.speeds = {{0, 2.0f}, {57, 1.0f}};
        CHECK(bac::GameFrames(cammy, 0, 58) == 30 && bac::GameFrames(cammy, 56, 58) == 2 && bac::GameFrames(cammy, 56, 57) == 1);
        // 0.5 up to 57, then 2: the frame from 56.75 reaches 58; stopping on
        // the change would take two.
        cammy.speeds = {{0, 0.5f}, {57, 2.0f}};
        CHECK(bac::GameFrames(cammy, 56.75f, 58) == 1);
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
        CHECK(!bac::ProjectileBoundary(model, action, first, last, &delay) && first == -1 && last == -1);
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
        CHECK(bac::ProjectileBoundary(unchanged, 0, a, b, &delay) && a == 27 && b == 28);
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
            bac::ProjectileBoundary(mutated, action, a, b, &delay);
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
    CHECK(!bac::ProjectileBoundary(untimed, 0, first, last, &delay));
    {
        // Ken's close MK (Omega): boxes 8 to 10 and 14 to 17, read after each update.
        bac::Script mk;
        mk.hits = {{8, 10}, {14, 17}};
        using P = bac::HitPhase;
        CHECK(bac::PhaseAt(mk, 7.5f) == P::Before && bac::PhaseAt(mk, 8.f) == P::Out && bac::PhaseAt(mk, 9.9f) == P::Out);
        CHECK(bac::PhaseAt(mk, 10.f) == P::Between && bac::PhaseAt(mk, 13.5f) == P::Between && bac::PhaseAt(mk, 14.f) == P::Out);
        CHECK(bac::PhaseAt(mk, 17.f) == P::Done && bac::PhaseAt(bac::Script{}, 9.f) == P::NoHit);
        mk.hits.push_back({-1, -1});
        CHECK(bac::PhaseAt(mk, 9.f) == P::Untimed);
    }
    {
        // El Fuerte's charges are three counters at three levels, and three pairs; Guile's two are alone.
        training::FighterData ric;
        const std::vector<std::string> names{"QUESA_L_tameL", "QUESA_L_tameM", "QUESA_L_tameH", "QUESA_M_tameL", "QUESA_M_tameM", "QUESA_M_tameH",
            "QUESA_H_tameL", "QUESA_H_tameM", "QUESA_H_tameH", "QUESA_EX_tameLM", "QUESA_EX_tameLH", "QUESA_EX_tameMH"};
        ric.chargeNames = std::make_shared<const std::vector<std::string>>(names);
        // As the game counts: 130, 37 and 185 frames held, each level stopping at its own requirement.
        const int held[3] = {130, 37, 185}, need[3] = {60, 120, 180};
        for (std::size_t i = 0; i < names.size(); ++i) ric.charges[i] = {i, training::ChargeBack, i < 9 ? need[i / 3] : 210, i < 9 ? (std::min)(held[i % 3], need[i / 3]) : 40 + static_cast<int>(i)};
        ric.chargeCount = names.size();
        auto groups = training::ChargeGroups(ric);
        const std::vector<std::size_t> lights{0, 3, 6}, mediums{1, 4, 7}, heavies{2, 5, 8};
        CHECK(training::ChargeGroupName(ric, lights) == "QUESA_tameL" && training::ChargeGroupName(ric, groups[3]) == "QUESA_EX_tameLM");
        CHECK(groups.size() == 6 && groups[0] == lights && groups[1] == mediums && groups[2] == heavies);
        // Nothing held yet: the same groups, as the frames a word changes tell the level from the counter.
        for (auto& charge : ric.charges) charge.held = 0;
        CHECK(training::ChargeGroups(ric).size() == 6);
        training::FighterData guile;
        guile.chargeNames = std::make_shared<const std::vector<std::string>>(std::vector<std::string>{"SONICBOOM", "SOMERSAULT"});
        guile.charges[0] = {0, training::ChargeBack, 50, 45}; guile.charges[1] = {1, training::ChargeDown, 55, 55}; guile.chargeCount = 2;
        CHECK(training::ChargeGroups(guile).size() == 2);
        // Juri's three button charges are not her fireball stock; no one else's are named so.
        const std::vector<std::string> juri{"CHARGE_L", "CHARGE_M", "CHARGE_H"};
        CHECK(training::HasFuhajinStock(&juri) && !training::HasFuhajinStock(ric.chargeNames.get()) && !training::HasFuhajinStock(guile.chargeNames.get()));
    }
    return 0;
}
