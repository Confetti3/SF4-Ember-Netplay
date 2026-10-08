#include "../training/ComboMoveMap.hxx"
#include "game_file_test_support.hxx"
#include "test_support.hxx"
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace sf4e;
using Scripts = std::vector<std::int32_t>;

static bool Refused(const Bytes& bytes, std::size_t size) {
    bcm::File model;
    std::string error;
    return !ReadExact(bytes, size, model, error) && !error.empty() && model.moves.empty();
}

// The script of every move the step may start, each once, in file order.
static Scripts StepScripts(const bcm::File& file, const combo::Step& step) {
    Scripts scripts;
    for (const auto& move : file.moves)
        if (move.script >= 0 && combo::Satisfies(file, move, step) && std::find(scripts.begin(), scripts.end(), move.script) == scripts.end())
            scripts.push_back(move.script);
    return scripts;
}

// One motion step: type, frames, input, forbid, match, count.
using StepRow = std::array<std::uint16_t, 6>;
struct MoveRow {
    const char* name;
    std::uint16_t input, flags;
    int motion, script;
    std::uint16_t state = 3, position = 0;
    std::uint32_t category = 0;
    std::int16_t meter = 0, revenge = 0;
    std::uint16_t rules = 0;
    std::int8_t ultra = 0;
};

// A command file laid out like a stock one: header, charges, motions and
// moves, each table followed by its name pointers, then the strings.
struct Builder {
    Bytes charges, motions, moves;
    std::vector<std::string> chargeNames, motionNames, moveNames;

    int Charge(std::uint16_t input, std::uint16_t flags, std::uint16_t frames, std::uint16_t window) {
        for (std::uint16_t v : {input, std::uint16_t(0), flags, frames, std::uint16_t(0), window}) Put16(charges, v);
        Put32(charges, static_cast<std::uint32_t>(chargeNames.size()));
        chargeNames.push_back("CHARGE");
        return static_cast<int>(chargeNames.size()) - 1;
    }
    int Motion(const std::vector<StepRow>& steps) {
        Put32(motions, static_cast<std::uint32_t>(steps.size()));
        for (const auto& step : steps) for (std::uint16_t v : step) Put16(motions, v);
        motions.resize(motions.size() + (16 - steps.size()) * 12);
        motionNames.push_back("MOTION");
        return static_cast<int>(motionNames.size()) - 1;
    }
    void Move(const MoveRow& row) {
        const std::size_t at = moves.size();
        float dist = row.position ? 1.25f : 0.0f;
        std::uint32_t distBits;
        std::memcpy(&distBits, &dist, 4);
        Put16(moves, row.input); Put16(moves, row.flags); Put16(moves, row.position); Put16(moves, row.rules);
        Put16(moves, 0); Put16(moves, row.state); Put16(moves, 0);
        moves.push_back(0); moves.push_back(0); moves.push_back(static_cast<std::uint8_t>(row.ultra)); moves.push_back(0);
        Put16(moves, 0); Put32(moves, distBits);
        Put16(moves, static_cast<std::uint16_t>(row.meter)); Put16(moves, 0);
        Put16(moves, static_cast<std::uint16_t>(row.revenge)); Put16(moves, 0);
        Put32(moves, static_cast<std::uint32_t>(row.motion)); Put32(moves, static_cast<std::uint32_t>(row.script));
        Put32(moves, row.category);
        moves.resize(at + 0x54);
        moveNames.push_back(row.name);
    }
    Bytes Build() const {
        Bytes out(0x38), strings;
        std::memcpy(out.data(), "#BCM", 4);
        const std::uint16_t fixed[] = {0xFFFE, 0x28, 1, 1};
        for (int i = 0; i < 4; ++i) { out[4 + 2 * i] = static_cast<std::uint8_t>(fixed[i]); out[5 + 2 * i] = static_cast<std::uint8_t>(fixed[i] >> 8); }
        const std::size_t counts[] = {chargeNames.size(), motionNames.size(), moveNames.size()};
        for (int i = 0; i < 3; ++i) out[0x10 + 2 * i] = static_cast<std::uint8_t>(counts[i]);
        const std::size_t stringsAt = 0x38 + charges.size() + motions.size() + moves.size() + 4 * (counts[0] + counts[1] + counts[2]);
        const Bytes* tables[] = {&charges, &motions, &moves};
        const std::vector<std::string>* names[] = {&chargeNames, &motionNames, &moveNames};
        for (int i = 0; i < 3; ++i) {
            Set32(out, 0x18 + 8 * i, static_cast<std::uint32_t>(out.size()));
            out.insert(out.end(), tables[i]->begin(), tables[i]->end());
            Set32(out, 0x1C + 8 * i, static_cast<std::uint32_t>(out.size()));
            for (const auto& name : *names[i]) {
                Put32(out, static_cast<std::uint32_t>(stringsAt + strings.size()));
                strings.insert(strings.end(), name.begin(), name.end());
                strings.push_back(0);
            }
        }
        // No cancel lists: both offsets point at the strings.
        Set32(out, 0x30, static_cast<std::uint32_t>(out.size()));
        Set32(out, 0x34, static_cast<std::uint32_t>(out.size()));
        out.insert(out.end(), strings.begin(), strings.end());
        return out;
    }
};

static Bytes Sample() {
    Builder b;
    const int back = b.Charge(0x08, 0x01, 50, 12), punches = b.Charge(0x1C0, 0x10, 60, 8);
    const int qcf = b.Motion({{{0, 12, 0x04, 0, 2, 0}}, {{0, 12, 0x14, 0, 2, 0}}, {{0, 12, 0x10, 0, 2, 0}}});
    const int dp = b.Motion({{{0, 8, 0x10, 0, 1, 0}}, {{0, 8, 0x04, 0, 1, 0}}, {{0, 12, 0x10, 0, 1, 0}}});
    const int dash = b.Motion({{{0, 8, 0x10, 0, 1, 0}}, {{0, 8, 0x01, 0, 1, 0}}, {{0, 8, 0x10, 0, 2, 0}}});
    const int boom = b.Motion({{{1, 0, static_cast<std::uint16_t>(back), 0, 0, 0}}, {{0, 10, 0x10, 0, 2, 0}}});
    const int spin = b.Motion({{{2, 24, 1, 0, 0, 0}}});
    const int slap = b.Motion(std::vector<StepRow>(5, StepRow{{3, 20, 0x1C0, 0, 0x10, 1}}));
    const int twice = b.Motion({{{0, 12, 0x04, 1, 2, 0}}, {{0, 12, 0x10, 0, 1, 0}}, {{0, 12, 0x04, 1, 2, 0}}, {{0, 16, 0x10, 0, 2, 0}}});
    const int hcf = b.Motion({{{0, 8, 0x08, 0, 1, 0}}, {{0, 8, 0x04, 1, 1, 0}}, {{0, 12, 0x10, 0, 2, 0}}});
    const int tap = b.Motion({{{1, 0, static_cast<std::uint16_t>(punches), 0, 0, 0}}});
    const int demon = b.Motion({{{0, 10, 0x40, 0, 0x20, 0}}, {{0, 10, 0x40, 0, 0x20, 0}}, {{0, 10, 0x10, 0, 2, 0}}});
    const int lift = b.Motion({{{2, 24, 1, 0, 0, 0}}, {{0, 5, 0x02, 0, 1, 0}}});
    const MoveRow rows[] = {
        {"5LP", 0x40, 0x1010, -1, 256, 3, 2, 0x1},
        {"5LPF", 0x40, 0x1010, -1, 262, 3, 1, 0x1},
        {"2MK", 0x404, 0x1411, -1, 272, 3, 0, 0x10},
        {"8HP", 0x100, 0x1010, -1, 276, 4, 0, 0x4},
        {"6MP", 0x90, 0x1412, -1, 320, 3, 0, 0x2},
        {"DASH", 0, 0, dash, 18, 3, 0, 0x80000},
        {"HADO_L", 0x40, 0x3010, qcf, 384, 3, 0, 0x200},
        {"HADO_M", 0x80, 0x3010, qcf, 385, 3, 0, 0x200},
        {"HADO_H", 0x100, 0x3010, qcf, 386, 3, 0, 0x200},
        {"HADO_EX", 0x1C0, 0x3050, qcf, 387, 3, 0, 0x1200, 250},
        {"AIR_TATSU_L", 0x200, 0x3010, qcf, 393, 4, 3, 0x200},
        {"SHORYU_L", 0x40, 0x3010, dp, 350, 3, 0, 0x200},
        {"SHORYU_L_2", 0x40, 0x3010, dp, 350, 3, 0, 0x200},
        {"BOOM_L", 0x40, 0x3010, boom, 396, 3, 0, 0x200},
        {"SPIN_L", 0x40, 0x3010, spin, 346, 3, 0, 0x200},
        {"SLAP_L", 0x40, 0x1010, slap, 400, 3, 0, 0x200},
        {"YOGA_L", 0x40, 0x3010, hcf, 402, 3, 0, 0x200},
        {"SUPER_L", 0x40, 0x3020, twice, 420, 3, 0, 0x400, 1000},
        {"ULTRA_2", 0x1C0, 0x3020, twice, 436, 3, 0, 0x800, 0, 200, 0x81, 1},
        {"THROW_F", 0x240, 0x1020, -1, 288, 1, 0, 0x40},
        {"FOCUS_LV1", 0x480, 0x2010, -1, 323, 3, 0, 0x80},
        {"TAP", 0x1C0, 0x2010, tap, 410, 3, 0, 0x200},
        {"DEMON", 0x200, 0x1010, demon, 450, 3, 0, 0x400, 1000},
        {"HADO_OFF", 0x40, 0x3010, qcf, -1, 3, 0, 0x200},
        {"LIFT_L", 0x200, 0x3010, lift, 448, 3, 0, 0x200},
    };
    for (const auto& row : rows) b.Move(row);
    return b.Build();
}

static const bcm::Move* Find(const bcm::File& file, const char* name) {
    for (const auto& move : file.moves) if (move.name == name) return &move;
    return nullptr;
}
static Scripts Starts(const bcm::File& file, const char* text) {
    combo::Step step;
    std::string error;
    CHECK(combo::ParseStep(text, step, error));
    return StepScripts(file, step);
}
static std::string Typed(const bcm::File& file, const char* name) {
    const bcm::Move* move = Find(file, name);
    CHECK(move);
    return combo::MoveNotation(*move);
}
// Every move the notation can say is satisfied by its own notation.
static std::size_t CheckRoundTrip(const bcm::File& file, const std::string& where) {
    std::size_t spelled = 0;
    for (const auto& move : file.moves) {
        combo::Step step, again;
        std::string error;
        if (!combo::MoveStep(move, step)) { CHECK(combo::MoveNotation(move).empty()); continue; }
        const std::string text = combo::Canonical(step);
        if (!combo::ParseStep(text, again, error) || combo::Canonical(again) != text || !combo::Satisfies(file, move, again)) {
            std::fprintf(stderr, "%s %s: \"%s\" does not start it\n", where.c_str(), move.name.c_str(), text.c_str());
            CHECK(false);
        }
        if (move.script >= 0) {
            const Scripts scripts = StepScripts(file, again);
            CHECK(std::find(scripts.begin(), scripts.end(), move.script) != scripts.end());
        }
        ++spelled;
    }
    return spelled;
}

int main() {
    const Bytes sample = Sample();
    bcm::File file;
    std::string error;
    CHECK(ReadExact(sample, sample.size(), file, error));
    CHECK(file.charges.size() == 2 && file.motions.size() == 11 && file.moves.size() == 25);
    CHECK(file.charges[0].input == bcm::Back && file.charges[0].frames == 50 && file.charges[0].window == 12);
    CHECK(file.motions[0].size() == 3 && file.motions[0][1].input == 0x14 && file.motions[0][1].match == 2 && file.motions[0][2].frames == 12);
    CHECK(file.motions[5].size() == 5 && file.motions[5][4].type == bcm::Repeat && file.motions[5][4].count == 1);

    {
        const bcm::Move& jab = *Find(file, "5LP");
        CHECK(jab.spelled && jab.motion.empty() && jab.buttons == combo::LP && jab.need == 1 && jab.press && !jab.release && !jab.air);
        CHECK(jab.position == bcm::CloseOnly && jab.dist == 1.25f && jab.script == 256 && jab.motionIndex == -1 && jab.state == 3);
        const bcm::Move& ex = *Find(file, "HADO_EX");
        CHECK(ex.motion == "236" && ex.buttons == combo::Punches && ex.need == 2 && ex.press && ex.release && ex.meter == 250);
        CHECK((ex.category & bcm::Ex) && !(ex.category & bcm::Super) && ex.script == 387);
        const bcm::Move& ultra = *Find(file, "ULTRA_2");
        CHECK(ultra.motion == "236236" && ultra.need == 3 && (ultra.category & bcm::Ultra) && (ultra.rules & bcm::NeedsUltra));
        CHECK(ultra.ultra == 1 && ultra.revenge == 200 && Find(file, "SUPER_L")->meter == 1000 && (Find(file, "SUPER_L")->category & bcm::Super));
        const bcm::Move& boom = *Find(file, "BOOM_L");
        CHECK(boom.charge && boom.motion == "46" && !boom.mash);
        CHECK(Find(file, "8HP")->air && Find(file, "AIR_TATSU_L")->air && Find(file, "AIR_TATSU_L")->position == bcm::AboveHeight);
        CHECK(Find(file, "SLAP_L")->mash && Find(file, "SPIN_L")->motion == "360" && Find(file, "HADO_OFF")->script == -1);
        const bcm::Move& release = *Find(file, "FOCUS_LV1");
        CHECK(release.release && !release.press && release.buttons == (combo::MP | combo::MK) && release.need == 1);
        CHECK(!Find(file, "DEMON")->spelled);
    }

    // A move to its notation.
    const char* const typed[][2] = {
        {"5LP", "cl.LP"}, {"5LPF", "far.LP"}, {"2MK", "2MK"}, {"8HP", "j.HP"}, {"6MP", "6MP"}, {"DASH", "66"}, {"HADO_L", "236LP"},
        {"HADO_H", "236HP"}, {"HADO_EX", "236PP"}, {"AIR_TATSU_L", "j.236LK"}, {"SHORYU_L", "623LP"}, {"BOOM_L", "[4]6LP"},
        {"SPIN_L", "360LP"}, {"SLAP_L", "5LP(mash)"}, {"YOGA_L", "41236LP"}, {"SUPER_L", "236236LP"}, {"ULTRA_2", "236236PPP"},
        {"THROW_F", "5LP+LK"}, {"FOCUS_LV1", "5]MP+MK["}, {"TAP", "5]P["}, {"DEMON", ""}, {"HADO_OFF", "236LP"}, {"LIFT_L", "360LK"}};
    for (const auto& pair : typed) {
        if (Typed(file, pair[0]) != pair[1]) std::fprintf(stderr, "%s is \"%s\"\n", pair[0], Typed(file, pair[0]).c_str());
        CHECK(Typed(file, pair[0]) == pair[1]);
    }
    CHECK(CheckRoundTrip(file, "sample") == 24);

    // A step to the scripts it may start.
    CHECK(Starts(file, "236P") == (Scripts{384, 385, 386}));
    CHECK(Starts(file, "236LP") == (Scripts{384}) && Starts(file, "236HP") == (Scripts{386}));
    CHECK(Starts(file, "236PP") == (Scripts{387}) && Starts(file, "236LP+MP") == (Scripts{387}));
    CHECK(Starts(file, "236]LP[") == (Scripts{384}) && Starts(file, "236]PP[") == (Scripts{387}));
    CHECK(Starts(file, "2MK") == (Scripts{272}) && Starts(file, "cr.MK") == (Scripts{272}) && Starts(file, "3MK") == (Scripts{272}));
    CHECK(Starts(file, "5LP") == (Scripts{256, 262}) && Starts(file, "LP") == (Scripts{256, 262}) && Starts(file, "st.LP") == (Scripts{256, 262}));
    CHECK(Starts(file, "cl.LP") == (Scripts{256}) && Starts(file, "far.LP") == (Scripts{262}));
    CHECK(Starts(file, "j.HP") == (Scripts{276}) && Starts(file, "HP").empty() && Starts(file, "j.LP").empty());
    CHECK(Starts(file, "6MP") == (Scripts{320}) && Starts(file, "4MP").empty() && Starts(file, "MP").empty());
    CHECK(Starts(file, "66") == (Scripts{18}) && Starts(file, "44").empty() && Starts(file, "6").empty());
    CHECK(Starts(file, "j.236LK") == (Scripts{393}) && Starts(file, "236LK").empty());
    // Two moves of one script count once.
    CHECK(Starts(file, "623LP") == (Scripts{350}) && Starts(file, "6236LP") == (Scripts{350}) && Starts(file, "623MP").empty());
    CHECK(Starts(file, "[4]6LP") == (Scripts{396}) && Starts(file, "[1]6P") == (Scripts{396}));
    CHECK(Starts(file, "46LP").empty() && Starts(file, "[2]6LP").empty() && Starts(file, "[4]236LP").empty());
    CHECK(Starts(file, "360LP") == (Scripts{346}) && Starts(file, "720LP").empty() && Starts(file, "360PPP").empty());
    CHECK(Starts(file, "360LK") == (Scripts{448}) && Starts(file, "360K") == (Scripts{448}) && Starts(file, "8LK").empty());
    CHECK(Starts(file, "LP(mash)") == (Scripts{400}) && Starts(file, "P(mash)") == (Scripts{400}) && Starts(file, "LK(mash)").empty());
    CHECK(Starts(file, "41236LP") == (Scripts{402}) && Starts(file, "426LP") == (Scripts{402}) && Starts(file, "4126P") == (Scripts{402}));
    // "Any" steps take a diagonal too; a step left out does not pass.
    CHECK(Starts(file, "1236LP") == (Scripts{402}) && Starts(file, "486LP").empty() && Starts(file, "4123LP").empty());
    // The Hadoken sits inside the super motion; the game picks by meter.
    CHECK(Starts(file, "236236LP") == (Scripts{384, 420}) && Starts(file, "236236P") == (Scripts{384, 385, 386, 420}));
    CHECK(Starts(file, "236236PPP") == (Scripts{387, 436}) && Starts(file, "236236KKK").empty());
    CHECK(Starts(file, "LP+LK") == (Scripts{288}) && Starts(file, "LP+MK").empty());
    CHECK(Starts(file, "]MP+MK[") == (Scripts{323}) && Starts(file, "MP+MK").empty() && Starts(file, "]MK[") == (Scripts{323}));
    CHECK(Starts(file, "]P[") == (Scripts{323, 410}) && Starts(file, "]PPP[") == (Scripts{410}) && Starts(file, "]HP[") == (Scripts{410}));
    CHECK(Starts(file, "FADC").empty() && Starts(file, "xx FADC44").empty());
    {
        // A step built by hand is judged the same way, whatever it holds.
        combo::Step odd;
        odd.motion = std::string(40, '6'); odd.buttons = combo::LP; odd.need = 1;
        CHECK(StepScripts(file, odd).empty());
        odd.motion = "2x6";
        CHECK(StepScripts(file, odd).empty());
        odd.motion.clear(); odd.charge = true;
        CHECK(StepScripts(file, odd).empty());
    }

    // Every shorter file is refused, and the model stays as it was.
    for (std::size_t size = 0; size < sample.size(); ++size) CHECK(Refused(sample, size));
    {
        bcm::File kept = file;
        CHECK(!ReadExact(sample, sample.size() - 1, kept, error) && kept.moves.size() == file.moves.size());
        CHECK(!bcm::Read(nullptr, 0, kept, error));
        Bytes large(bcm::MaxBytes + 1);
        std::copy(sample.begin(), sample.end(), large.begin());
        CHECK(Refused(large, large.size()));
    }
    const std::uint32_t chargeAt = sample[0x18] | sample[0x19] << 8, motionAt = sample[0x20] | sample[0x21] << 8,
        moveAt = sample[0x28] | sample[0x29] << 8, nameAt = sample[0x2C] | sample[0x2D] << 8;
    const auto broken = [&](std::size_t at, std::uint32_t value, int width) {
        Bytes bytes = sample;
        for (int i = 0; i < width; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
        return Refused(bytes, bytes.size());
    };
    CHECK(chargeAt == 0x38 && motionAt && moveAt && nameAt);
    CHECK(broken(0, 'X', 1));
    for (std::size_t count : {0x10, 0x12, 0x14}) CHECK(broken(count, 0xFFFF, 2));
    for (std::size_t offset : {0x18, 0x20, 0x28, 0x2C}) {
        CHECK(broken(offset, 0xFFFFFFFF, 4) && broken(offset, 0x7FFFFFFF, 4) && broken(offset, static_cast<std::uint32_t>(sample.size()), 4));
        CHECK(broken(offset, static_cast<std::uint32_t>(sample.size()) - 8, 4));
    }
    CHECK(broken(motionAt, 17, 4) && broken(motionAt, 0xFFFFFFFF, 4));          // step count
    CHECK(broken(motionAt + 4, 4, 2));                                          // step type
    CHECK(broken(moveAt + 0x20, 11, 4) && broken(moveAt + 0x20, 0xFFFFFFFE, 4)); // motion index
    CHECK(broken(moveAt + 6 * 0x54 + 0x20, 0x7FFFFFFF, 4));
    CHECK(broken(moveAt + 0x24, 0xFFFFFFFE, 4));                                // script
    CHECK(broken(nameAt, 0xFFFFFFFF, 4) && broken(nameAt, static_cast<std::uint32_t>(sample.size()), 4));
    CHECK(broken(sample.size() - 1, 'x', 1));                                   // the last name loses its NUL
    CHECK(broken(sample.size() - 2, 0x07, 1) && broken(sample.size() - 2, 0xC3, 1));
    {
        // A name longer than any real one.
        Bytes bytes = sample;
        bytes.pop_back();
        bytes.insert(bytes.end(), bcm::MaxName, 'A');
        bytes.push_back(0);
        CHECK(Refused(bytes, bytes.size()));
        // A charge step that names no charge is read, and starts nothing.
        bytes = sample;
        bytes[motionAt + 3 * 0xC4 + 4 + 4] = 0xFF; bytes[motionAt + 3 * 0xC4 + 4 + 5] = 0xFF;
        bcm::File odd;
        CHECK(ReadExact(bytes, bytes.size(), odd, error) && !Find(odd, "BOOM_L")->spelled);
        CHECK(Starts(odd, "[4]6LP").empty() && Starts(odd, "6LP").empty());
    }
    // Any single wrong byte is either refused or read without leaving the buffer.
    for (std::size_t at = 0; at < sample.size(); ++at) for (std::uint8_t value : {std::uint8_t(0x00), std::uint8_t(0x7F), std::uint8_t(0xFF)}) {
        Bytes bytes = sample;
        bytes[at] = value;
        bcm::File odd;
        if (!ReadExact(bytes, bytes.size(), odd, error)) continue;
        for (const auto& move : odd.moves) combo::MoveNotation(move);
        for (const char* text : {"236P", "[4]6LP", "360LP", "5LP", "j.HP", "66", "LP(mash)", "]PPP["}) Starts(odd, text);
    }

    if (const char* game = std::getenv("SF4E_GAME_DIR")) {
        namespace fs = std::filesystem;
        // The game reads these twelve fighters from the newer folder.
        const std::string newer = " RYU GUL BLR VEG JHA CMY DAN GUY DDL RLN PSN DCP ";
        const fs::path older = fs::path(game) / "patch_ae2_tu2/battle/regulation/ae2_110";
        std::map<std::string, bcm::File> fighters;
        std::size_t moves = 0, spelled = 0;
        std::error_code ec;
        for (fs::directory_iterator it(older, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string code = it->path().filename().string();
            const fs::path folder = newer.find(" " + code + " ") == std::string::npos ? older : fs::path(game) / "patch_ae2_tu3/battle/regulation/ae2_111";
            std::ifstream stream(folder / code / (code + ".bcm"), std::ios::binary);
            const Bytes stock((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            bcm::File& model = fighters[code];
            if (!ReadExact(stock, stock.size(), model, error)) {
                std::fprintf(stderr, "%s: %s\n", code.c_str(), error.c_str());
                CHECK(false);
            }
            CHECK(!model.moves.empty());
            moves += model.moves.size();
            spelled += CheckRoundTrip(model, code);
        }
        CHECK(fighters.size() == 44);

        const bcm::File& ryu = fighters["RYU"];
        const char* const known[][2] = {
            {"HADOLEN_L", "236LP"}, {"SYORYUKEN_L", "623LP"}, {"TATSUMAKI_L", "214LK"}, {"SHINKU_HADOLEN_L", "236236LP"},
            {"METU_HADOKEN", "236236PPP"}, {"METU_SHOURYUU", "236236KKK"}, {"6MP", "6MP"}, {"FIRE_HADOLEN", "236PP"},
            {"AIR_TATSUMAKI_L", "j.214LK"}, {"2MK", "2MK"}, {"5LP", "cl.LP"}, {"5LPF", "far.LP"}, {"DASH", "66"}, {"8HP", "j.HP"}};
        for (const auto& pair : known) CHECK(Typed(ryu, pair[0]) == pair[1]);
        CHECK(Find(ryu, "HADOLEN_L")->script == 384 && Find(ryu, "SYORYUKEN_L")->script == 350 && Find(ryu, "TATSUMAKI_L")->script == 390);
        CHECK(Find(ryu, "6MP")->input == 0x90 && Find(ryu, "6MP")->flags == 0x1412 && Find(ryu, "6MP")->script == 320);
        const bcm::Move& super = *Find(ryu, "SHINKU_HADOLEN_L");
        CHECK(super.meter == 1000 && super.script == 420 && (super.category & bcm::Super));
        const bcm::Move& ultra1 = *Find(ryu, "METU_HADOKEN"), & ultra2 = *Find(ryu, "METU_SHOURYUU");
        CHECK(ultra1.rules == 0x81 && ultra1.revenge == 200 && ultra1.ultra == 0 && ultra1.script == 436 && (ultra1.category & bcm::Ultra));
        CHECK(ultra2.ultra == 1 && ultra2.script == 440);
        const bcm::Move& ex = *Find(ryu, "FIRE_HADOLEN");
        CHECK(ex.meter == 250 && ex.script == 387 && (ex.category & bcm::Ex));
        CHECK(Starts(ryu, "236P") == (Scripts{384, 385, 386}) && Starts(ryu, "236PP") == (Scripts{387}));
        CHECK(Starts(ryu, "2MK") == (Scripts{272}) && Starts(ryu, "cl.LP") == (Scripts{256, 300}) && Starts(ryu, "6MP") == (Scripts{320}));
        CHECK(Starts(ryu, "623LP") == (Scripts{350}) && Starts(ryu, "236236PPP") == (Scripts{387, 436}) && Starts(ryu, "66") == (Scripts{18}));
        CHECK(Starts(ryu, "236236KKK") == (Scripts{440}));

        const bcm::File& guile = fighters["GUL"];
        const bcm::Move& boom = *Find(guile, "SONICBOOM_L");
        CHECK(Typed(guile, "SONICBOOM_L") == "[4]6LP" && boom.script == 396 && guile.motions[boom.motionIndex][0].type == bcm::ChargeStep);
        const bcm::Charge& hold = guile.charges[guile.motions[boom.motionIndex][0].input];
        CHECK(hold.input == bcm::Back && (hold.flags & 0xF) == 1 && hold.frames == 50 && hold.window == 12);
        CHECK(Typed(guile, "SOMERSAULT_L") == "[2]8LK" && Typed(guile, "SONIC_HURRICANE") == "[4]646PPP");
        CHECK(Starts(guile, "[4]6P").size() == 3 && Starts(guile, "[4]6P")[0] == 396 && Starts(guile, "46P").empty());

        const bcm::File& zangief = fighters["ZGF"];
        CHECK(Typed(zangief, "SCREW_L") == "360LP" && Find(zangief, "SCREW_L")->script == 346);
        CHECK(Typed(zangief, "UC_SCREW") == "720PPP" && Find(zangief, "UC_SCREW")->script == 470);
        const bcm::Motion& one = zangief.motions[Find(zangief, "SCREW_L")->motionIndex], & two = zangief.motions[Find(zangief, "UC_SCREW")->motionIndex];
        CHECK(one[0].type == bcm::Rotation && one[0].frames == 24 && two[0].input == 2 && two[0].frames == 32);
        CHECK(Starts(zangief, "360LP") == (Scripts{346}) && Starts(fighters["HUG"], "360LK") == (Scripts{448}));
        std::printf("%zu fighters, %zu moves, %zu start from their own notation\n", fighters.size(), moves, spelled);
    }
    return 0;
}
