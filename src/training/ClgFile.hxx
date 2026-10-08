#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "ByteReader.hxx"

// The game's trial files (<CODE>.clg, <CODE>_swan.clg): 24 levels per fighter,
// each a list of steps the player must land in order. Owns only the model and
// its byte layout; it knows nothing of the game or the overlay.
namespace sf4e { namespace clg {
// The trial menu always lists 24 levels, so a file meant for the game holds
// exactly GameLevels levels. Its task list shows GameSteps rows and does not
// scroll; a level may hold more steps, which the player cannot read on
// screen. Read and Write accept other counts.
constexpr std::size_t GameLevels = 24, GameSteps = 8;
// A text id or level name fills a fixed slot and keeps one byte for its NUL.
constexpr std::size_t MaxText = 31, MaxName = 43;
// The game reads a step's criteria count as a signed 16-bit value.
constexpr std::size_t MaxCriteria = 0x7FFF;
// Files come from other players, so the whole file is bounded. The largest
// stock file is under 40 KiB.
constexpr std::size_t MaxBytes = 1024 * 1024;

enum StepType : std::int16_t { Script = 0, Attack = 1 };
enum DummyState : std::int32_t { Stand = 0, Crouch = 1, Jump = 2, StandAttack = 3, CrouchAttack = 4 };

// screen, help: text ids for the task row and the help menu, slot 0 stance or
// "EX", 1 strength, 2 the move, 3 a trailing note; an unused slot is empty.
// criteria: the step is met by any of these ids, hit-data set indices of the
// fighter's .bac for Attack, script indices for Script.
struct Step {
    std::array<std::string, 4> screen, help;
    std::int16_t type = Attack;
    std::vector<std::int32_t> criteria;
};

// ultra: 0 selects Ultra 1, 1 Ultra 2. dummy: a DummyState. The game is not
// seen reading the remaining fields; their defaults are what RYU.clg holds for
// a fighter without menu positions, and a file edited from a stock one keeps
// the stock values.
struct Level {
    std::string name;
    std::uint16_t menuA = 0xFFFF, menuB = 0xFFFF;
    std::int32_t field30 = -1, field40 = 0, field4C = 0, field50 = -2;
    std::int32_t ultra = 0;
    std::array<std::int32_t, 6> setup{{0, 0x7FFFFFFF, -1, 26, 1, 0}};
    std::int32_t dummy = Stand, flags = 1;
    std::vector<Step> steps;
};

// Trailing records a few stock files carry. Nothing is seen reading them.
struct Extra { std::int32_t a = 0; std::uint16_t b = 0, c = 0; std::int32_t d = 0, e = 0; };

struct File { std::vector<Level> levels; std::vector<Extra> extras; };

inline bool operator==(const Step& a, const Step& b) {
    return a.screen == b.screen && a.help == b.help && a.type == b.type && a.criteria == b.criteria;
}
inline bool operator==(const Level& a, const Level& b) {
    return a.name == b.name && a.menuA == b.menuA && a.menuB == b.menuB && a.field30 == b.field30 && a.field40 == b.field40
        && a.field4C == b.field4C && a.field50 == b.field50 && a.ultra == b.ultra && a.setup == b.setup && a.dummy == b.dummy
        && a.flags == b.flags && a.steps == b.steps;
}
inline bool operator==(const Extra& a, const Extra& b) { return a.a == b.a && a.b == b.b && a.c == b.c && a.d == b.d && a.e == b.e; }
inline bool operator==(const File& a, const File& b) { return a.levels == b.levels && a.extras == b.extras; }

namespace detail {
constexpr std::size_t HeaderSize = 0x40, LevelSize = 0x70, SetupSize = 0x30, TaskSize = 0x10, StepSize = 0x108, ExtraSize = 0x10,
    TextSize = 0x20, NameSize = 0x2C;

inline bool Writable(const std::string& text, std::size_t most) {
    if (text.size() > most) return false;
    for (unsigned char c : text) if (c < 0x20 || c > 0x7E) return false;
    return true;
}
inline void Put16(std::vector<unsigned char>& out, std::uint32_t value) {
    out.push_back(static_cast<unsigned char>(value));
    out.push_back(static_cast<unsigned char>(value >> 8));
}
inline void Put32(std::vector<unsigned char>& out, std::uint32_t value) { Put16(out, value); Put16(out, value >> 16); }
inline void PutText(std::vector<unsigned char>& out, const std::string& text, std::size_t slot) {
    out.insert(out.end(), text.begin(), text.end());
    out.insert(out.end(), slot - text.size(), 0);
}
}

// Reads a whole .clg file. Returns false with a reason and leaves model alone
// when the bytes are not a file Write could have produced field for field.
inline bool Read(const unsigned char* bytes, std::size_t size, File& model, std::string& error) {
    using namespace detail;
    const auto fail = [&](const std::string& why) { error = why; return false; };
    if (!bytes || size < HeaderSize) return fail("the file is shorter than its header");
    if (size > MaxBytes) return fail("the file is too large");
    const ByteReader in{bytes, size};
    if (bytes[0] != '#' || bytes[1] != 'C' || bytes[2] != 'L' || bytes[3] != 'G') return fail("the file is not a trial file");
    if (in.U16(4) != 0xFFFE || in.U16(6) != HeaderSize || in.U16(8) != 1 || !in.Zero(0x0A, 0x26)) return fail("the header is not a known version");
    const std::uint32_t levelCount = in.U32(0x30), extraCount = in.U32(0x38), extraOffset = in.U32(0x3C);
    if (in.U32(0x34) != HeaderSize) return fail("the level table does not follow the header");
    if (levelCount > (size - HeaderSize) / LevelSize) return fail("the level table runs past the end of the file");

    File file;
    file.levels.resize(levelCount);
    // Steps and criteria of a real file never share bytes, so together they
    // fit in the file. This keeps offsets that point many levels at the same
    // bytes from growing the model past the file's own size.
    std::uint64_t used = 0;
    for (std::uint32_t i = 0; i < levelCount; ++i) {
        const std::string where = "level " + std::to_string(i + 1);
        const std::uint64_t base = HeaderSize + std::uint64_t(i) * LevelSize;
        Level& level = file.levels[i];
        if (!in.Text(base, NameSize, level.name)) return fail(where + ": the name is not text");
        level.menuA = in.U16(base + 0x2C);
        level.menuB = in.U16(base + 0x2E);
        level.field30 = in.I32(base + 0x30);
        level.field40 = in.I32(base + 0x40);
        level.field4C = in.I32(base + 0x4C);
        level.field50 = in.I32(base + 0x50);
        level.ultra = in.I32(base + 0x54);
        if (!in.Zero(base + 0x34, 12) || !in.Zero(base + 0x44, 8) || !in.Zero(base + 0x60, 8)) return fail(where + ": a reserved field is set");
        if (in.U32(base + 0x58) != 1 || in.U32(base + 0x68) != 1) return fail(where + ": it does not have one setup and one task list");

        const std::uint64_t setup = base + in.U32(base + 0x5C);
        if (!in.Has(setup, SetupSize)) return fail(where + ": the setup runs past the end of the file");
        if (!in.Zero(setup, 4) || !in.Zero(setup + 28, 20)) return fail(where + ": a reserved setup field is set");
        for (std::size_t k = 0; k < level.setup.size(); ++k) level.setup[k] = in.I32(setup + 4 + 4 * k);

        const std::uint64_t task = base + in.U32(base + 0x6C);
        if (!in.Has(task, TaskSize)) return fail(where + ": the task list runs past the end of the file");
        level.dummy = in.I32(task);
        level.flags = in.I32(task + 4);
        const std::uint32_t stepCount = in.U32(task + 8);
        const std::uint64_t steps = task + in.U32(task + 12);
        if (stepCount > size / StepSize || !in.Has(steps, std::uint64_t(stepCount) * StepSize)) return fail(where + ": the steps run past the end of the file");
        if ((used += std::uint64_t(stepCount) * StepSize) > size) return fail(where + ": the steps overlap");
        level.steps.resize(stepCount);
        for (std::uint32_t j = 0; j < stepCount; ++j) {
            const std::string step = where + " step " + std::to_string(j + 1);
            const std::uint64_t at = steps + std::uint64_t(j) * StepSize;
            Step& out = level.steps[j];
            for (std::size_t k = 0; k < 4; ++k) {
                if (!in.Text(at + k * TextSize, TextSize, out.screen[k]) || !in.Text(at + (4 + k) * TextSize, TextSize, out.help[k])) return fail(step + ": a text id is not text");
            }
            out.type = static_cast<std::int16_t>(in.U16(at + 0x100));
            const std::uint32_t count = in.U16(at + 0x102);
            const std::uint64_t criteria = at + in.U32(at + 0x104);
            if (count > MaxCriteria) return fail(step + ": too many criteria");
            if (!in.Has(criteria, std::uint64_t(count) * 4)) return fail(step + ": the criteria run past the end of the file");
            if ((used += std::uint64_t(count) * 4) > size) return fail(step + ": the criteria overlap");
            out.criteria.resize(count);
            for (std::uint32_t k = 0; k < count; ++k) out.criteria[k] = in.I32(criteria + 4 * std::uint64_t(k));
        }
    }
    if (extraCount) {
        const std::uint64_t extras = 0x38 + std::uint64_t(extraOffset);
        if (extraCount > size / ExtraSize || !in.Has(extras, std::uint64_t(extraCount) * ExtraSize)) return fail("the extra records run past the end of the file");
        file.extras.resize(extraCount);
        for (std::uint32_t i = 0; i < extraCount; ++i) {
            const std::uint64_t at = extras + std::uint64_t(i) * ExtraSize;
            Extra& extra = file.extras[i];
            extra.a = in.I32(at);
            extra.b = in.U16(at + 4);
            extra.c = in.U16(at + 6);
            extra.d = in.I32(at + 8);
            extra.e = in.I32(at + 12);
        }
    } else if (extraOffset) return fail("the header has an extra offset without extra records");
    model = std::move(file);
    return true;
}

// Lays the file out the way the stock files are: header, levels, setups, task
// lists, every step in level order, every criteria list in the same order,
// then the extra records. Returns false with a reason and leaves bytes alone
// when the model does not fit the format.
inline bool Write(const File& model, std::vector<unsigned char>& bytes, std::string& error) {
    using namespace detail;
    const auto fail = [&](const std::string& why) { error = why; return false; };
    const std::size_t levelCount = model.levels.size();
    std::uint64_t stepCount = 0, criteriaCount = 0;
    if (levelCount > MaxBytes / LevelSize || model.extras.size() > MaxBytes / ExtraSize) return fail("the file would be too large");
    for (std::size_t i = 0; i < levelCount; ++i) {
        const Level& level = model.levels[i];
        const std::string where = "level " + std::to_string(i + 1);
        if (!Writable(level.name, MaxName)) return fail(where + ": the name is too long or not plain text");
        stepCount += level.steps.size();
        for (std::size_t j = 0; j < level.steps.size(); ++j) {
            const Step& step = level.steps[j];
            const std::string at = where + " step " + std::to_string(j + 1);
            for (std::size_t k = 0; k < 4; ++k) {
                if (!Writable(step.screen[k], MaxText) || !Writable(step.help[k], MaxText)) return fail(at + ": a text id is too long or not plain text");
            }
            if (step.criteria.size() > MaxCriteria) return fail(at + ": too many criteria");
            criteriaCount += step.criteria.size();
        }
        if (stepCount > MaxBytes / StepSize) return fail("the file would be too large");
    }
    const std::uint64_t setups = HeaderSize + levelCount * LevelSize, tasks = setups + levelCount * SetupSize, steps = tasks + levelCount * TaskSize,
        criteria = steps + stepCount * StepSize, extras = criteria + criteriaCount * 4, total = extras + model.extras.size() * ExtraSize;
    if (total > MaxBytes) return fail("the file would be too large");

    std::vector<unsigned char> out;
    out.reserve(static_cast<std::size_t>(total));
    PutText(out, "#CLG", 4);
    Put16(out, 0xFFFE);
    Put16(out, HeaderSize);
    Put16(out, 1);
    out.insert(out.end(), 0x26, 0);
    Put32(out, static_cast<std::uint32_t>(levelCount));
    Put32(out, HeaderSize);
    Put32(out, static_cast<std::uint32_t>(model.extras.size()));
    Put32(out, model.extras.empty() ? 0 : static_cast<std::uint32_t>(extras - 0x38));
    for (std::size_t i = 0; i < levelCount; ++i) {
        const Level& level = model.levels[i];
        const std::uint64_t base = HeaderSize + i * LevelSize;
        PutText(out, level.name, NameSize);
        Put16(out, level.menuA);
        Put16(out, level.menuB);
        Put32(out, static_cast<std::uint32_t>(level.field30));
        out.insert(out.end(), 12, 0);
        Put32(out, static_cast<std::uint32_t>(level.field40));
        out.insert(out.end(), 8, 0);
        Put32(out, static_cast<std::uint32_t>(level.field4C));
        Put32(out, static_cast<std::uint32_t>(level.field50));
        Put32(out, static_cast<std::uint32_t>(level.ultra));
        Put32(out, 1);
        Put32(out, static_cast<std::uint32_t>(setups + i * SetupSize - base));
        out.insert(out.end(), 8, 0);
        Put32(out, 1);
        Put32(out, static_cast<std::uint32_t>(tasks + i * TaskSize - base));
    }
    for (const Level& level : model.levels) {
        Put32(out, 0);
        for (std::int32_t value : level.setup) Put32(out, static_cast<std::uint32_t>(value));
        out.insert(out.end(), 20, 0);
    }
    std::uint64_t stepAt = steps, criteriaAt = criteria;
    for (std::size_t i = 0; i < levelCount; ++i) {
        const Level& level = model.levels[i];
        Put32(out, static_cast<std::uint32_t>(level.dummy));
        Put32(out, static_cast<std::uint32_t>(level.flags));
        Put32(out, static_cast<std::uint32_t>(level.steps.size()));
        Put32(out, static_cast<std::uint32_t>(stepAt - (tasks + i * TaskSize)));
        stepAt += level.steps.size() * StepSize;
    }
    stepAt = steps;
    for (const Level& level : model.levels) for (const Step& step : level.steps) {
        for (const std::string& text : step.screen) PutText(out, text, TextSize);
        for (const std::string& text : step.help) PutText(out, text, TextSize);
        Put16(out, static_cast<std::uint16_t>(step.type));
        Put16(out, static_cast<std::uint32_t>(step.criteria.size()));
        Put32(out, static_cast<std::uint32_t>(criteriaAt - stepAt));
        stepAt += StepSize;
        criteriaAt += step.criteria.size() * 4;
    }
    for (const Level& level : model.levels) for (const Step& step : level.steps) {
        for (std::int32_t id : step.criteria) Put32(out, static_cast<std::uint32_t>(id));
    }
    for (const Extra& extra : model.extras) {
        Put32(out, static_cast<std::uint32_t>(extra.a));
        Put16(out, extra.b);
        Put16(out, extra.c);
        Put32(out, static_cast<std::uint32_t>(extra.d));
        Put32(out, static_cast<std::uint32_t>(extra.e));
    }
    bytes = std::move(out);
    return true;
}
}}
