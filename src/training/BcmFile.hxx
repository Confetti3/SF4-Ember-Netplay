#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "ByteReader.hxx"

// The game's command files (<CODE>.bcm): every move a fighter can start, the
// stick motion and buttons that start it, and the script it plays. On the one
// capture checked (Ken) the script index was the action id the training
// session samples; TrainingRuntime treats that as an assumption. Owns only the
// model and its byte layout; it knows nothing of the game or the overlay.
namespace sf4e { namespace bcm {
// The largest stock file is under 24 KiB and its longest name is 24 bytes.
constexpr std::size_t MaxBytes = 1024 * 1024, MaxName = 63;

// Input bits as the file stores them, written for a fighter facing right.
constexpr unsigned Neutral = 0x01, Up = 0x02, Down = 0x04, Back = 0x08, Forward = 0x10, Directions = 0x1F, Buttons = 0xFC0;
// Move::state and Move::foe.
constexpr unsigned Standing = 1, Crouching = 2, Airborne = 4;
// Move::category.
constexpr std::uint32_t Throw = 0x40, Focus = 0x80, Special = 0x200, Super = 0x400, Ultra = 0x800, Ex = 0x1000, TargetCombo = 0x2000;
// Move::rules: the selected ultra must be Move::ultra.
constexpr unsigned NeedsUltra = 0x80;
// Move::position.
constexpr unsigned FarOnly = 1, CloseOnly = 2, AboveHeight = 3, BelowHeight = 4;
enum StepType : std::uint16_t { Direction = 0, ChargeStep = 1, Rotation = 2, Repeat = 3 };

// input: what to hold. flags: low nibble 1 = any direction bit of input, 2 =
// all of them. frames: how long. window: frames the charge stays usable after
// it is let go.
struct Charge { std::uint16_t input = 0, forbid = 0, flags = 0, frames = 0, grace = 0, window = 0; };
// input: a mask for Direction and Repeat, a charge index for ChargeStep, the
// number of full turns for Rotation. match: low nibble 1 = any direction bit
// of input, 2 = exactly input, 0 = always. frames: how far back the game
// searches for this step. count: presses a Repeat needs.
struct MotionStep { std::uint16_t type = 0, frames = 0, input = 0, forbid = 0, match = 0, count = 0; };
using Motion = std::vector<MotionStep>;

struct Move {
    std::string name;
    // The command, in the form ComboBook's Step uses. spelled is false when
    // that form cannot say it (buttons inside the motion, a move with nothing
    // to type); the other fields are then a best effort.
    // motion: numpad digits, "360" or "720"; with charge the first digit is
    //   held. Empty when the move has no direction of its own.
    // buttons/need: pad bits (LP 0x10, MP 0x20, HP 0x400, LK 0x40, MK 0x80,
    //   HK 0x800), of which `need` together.
    // press/release: the move starts when the buttons go down, come up, or either.
    // air: the move starts only in the air.
    std::string motion;
    bool spelled = false, charge = false, mash = false, press = false, release = false, air = false;
    unsigned buttons = 0;
    int need = 0;
    // The file's own fields. flags: 0x00F direction test and 0x0F0 button
    // test on input, 0xF000 which button edge. position: FarOnly and CloseOnly
    // compare the distance to the foe with dist, the height rules the own
    // height. meter, revenge: what the move needs of the super and revenge
    // bars (250 EX, 1000 super; 200 ultra). ultra: 0 = Ultra 1, 1 = Ultra 2.
    // script: index into the script table of the fighter's .bac, -1 on a
    // disabled move.
    std::uint16_t input = 0, flags = 0, position = 0, rules = 0, state = 0, foe = 0;
    std::int8_t stance = 0, ultra = 0;
    float dist = 0;
    std::int16_t meter = 0, revenge = 0;
    std::int32_t motionIndex = -1, script = -1;
    std::uint32_t category = 0;
};

// moves are in file order, which is the game's priority: of the moves an
// input satisfies, the last one wins.
struct File { std::vector<Charge> charges; std::vector<Motion> motions; std::vector<Move> moves; };

// The numpad digit of a direction mask, or 0 when the mask is not one direction.
inline char Numpad(unsigned mask) {
    switch (mask & Directions) {
    case Neutral: return '5'; case Up: return '8'; case Down: return '2'; case Back: return '4'; case Forward: return '6';
    case Up | Back: return '7'; case Up | Forward: return '9'; case Down | Back: return '1'; case Down | Forward: return '3';
    default: return 0;
    }
}
// The direction mask of a numpad digit, or 0.
inline unsigned DirectionMask(char digit) {
    static const unsigned masks[] = {Down | Back, Down, Down | Forward, Back, Neutral, Forward, Up | Back, Up, Up | Forward};
    return digit >= '1' && digit <= '9' ? masks[digit - '1'] : 0;
}
// The file's button bits as pad bits.
inline unsigned PadButtons(unsigned input) {
    return (input & 0x40 ? 0x10u : 0) | (input & 0x80 ? 0x20u : 0) | (input & 0x100 ? 0x400u : 0) |
        (input & 0x200 ? 0x40u : 0) | (input & 0x400 ? 0x80u : 0) | (input & 0x800 ? 0x800u : 0);
}

namespace detail {
constexpr std::size_t HeaderSize = 0x38, ChargeSize = 0x10, MotionSize = 0xC4, StepSize = 0x0C, MoveSize = 0x54, MaxSteps = 16;

// The motion as players write it. The game's shortcuts make the stored steps
// shorter: a Shoryuken is stored as forward, down, forward.
inline std::string Spell(const std::string& raw, bool charged) {
    static const char* const plain[][2] = {
        {"656", "66"}, {"454", "44"}, {"626", "623"}, {"2626", "236236"}, {"624", "63214"}, {"2424", "214214"}, {"424", "421"},
        {"426", "41236"}, {"624624", "6321463214"}, {"426426", "4123641236"}, {"1269", "12369"}, {"858", "88"}, {"252", "22"},
        {"25252", "222"}, {"52", "2"}, {"4268", "412368"}};
    if (charged) return raw == "618" ? "319" : raw;
    for (const auto& entry : plain) if (raw == entry[0]) return entry[1];
    return raw;
}

// Fills the Step-shaped fields of a move from its raw ones.
inline void Describe(const File& file, Move& move) {
    bool ok = true;
    char held = 0;
    std::string raw;
    if (move.motionIndex >= 0) {
        const Motion& steps = file.motions[static_cast<std::size_t>(move.motionIndex)];
        if (!steps.empty() && steps[0].type == Rotation) {
            // A turn that must end on a direction is still written as the turn.
            move.motion = steps[0].input <= 1 ? "360" : "720";
        } else for (std::size_t i = 0; i < steps.size(); ++i) {
            const MotionStep& step = steps[i];
            if (step.type == Repeat) move.mash = true;
            else if (step.type == ChargeStep) {
                if (i || step.input >= file.charges.size()) ok = false;
                // A held button is let go to start the move; it is no stick motion.
                else if (!(file.charges[step.input].input & Buttons)) {
                    held = Numpad(file.charges[step.input].input);
                    ok = ok && held;
                }
            } else if (step.type != Direction || (step.input & Buttons)) ok = false;
            // A step without a direction test passes on any input.
            else if (step.match & 0xF) {
                const char digit = Numpad(step.input);
                if (digit) raw += digit; else ok = false;
            }
        }
        if (move.mash) raw.clear();
        if (move.motion.empty()) move.motion = (held ? std::string(1, held) : "") + Spell(raw, held != 0);
    }
    const unsigned how = move.flags & 0xFu, test = move.flags & 0xF0u, stick = move.input & Directions;
    if ((how == 1 || how == 2) && stick != Neutral) {
        const char digit = Numpad(stick);
        if (!digit || move.motion == "360" || move.motion == "720") ok = false;
        else move.motion += digit;
    }
    move.charge = held != 0;
    if (move.charge && move.motion.size() < 2) ok = false;

    move.buttons = test ? PadButtons(move.input) : 0;
    int bits = 0;
    for (unsigned b = move.buttons; b; b &= b - 1) ++bits;
    if (test == 0x10) move.need = 1;
    else if (test == 0x50) move.need = 2;
    else if (test == 0x20) move.need = bits;
    else if (test) ok = false;
    if (move.buttons) {
        move.release = (move.flags & 0x2000) != 0;
        move.press = (move.flags & 0x5000) != 0;
    }
    move.air = (move.state & (Standing | Crouching | Airborne)) == Airborne;
    move.spelled = ok && move.motion.size() <= 10 && (move.buttons || !move.motion.empty()) && (!move.mash || move.buttons);
}
}

// Reads a whole .bcm file: the charges, motions and moves. The cancel lists
// and the names of charges and motions are not read. Returns false with a
// reason and leaves model alone when a count, offset or index does not fit.
inline bool Read(const std::uint8_t* bytes, std::size_t size, File& model, std::string& error) {
    using namespace detail;
    static_assert(sizeof(float) == 4, "the distance is a 32-bit float");
    const auto fail = [&](const std::string& why) { error = why; return false; };
    if (!bytes || size < HeaderSize) return fail("the file is shorter than its header");
    if (size > MaxBytes) return fail("the file is too large");
    if (std::memcmp(bytes, "#BCM", 4) != 0) return fail("the file is not a command file");
    const ByteReader in{bytes, size};
    const std::size_t chargeCount = in.U16(0x10), motionCount = in.U16(0x12), moveCount = in.U16(0x14);
    const std::uint64_t chargeAt = in.U32(0x18), motionAt = in.U32(0x20), moveAt = in.U32(0x28), nameAt = in.U32(0x2C);
    if (chargeCount && !in.Has(chargeAt, chargeCount * ChargeSize)) return fail("the charge table runs past the end of the file");
    if (motionCount && !in.Has(motionAt, motionCount * MotionSize)) return fail("the motion table runs past the end of the file");
    if (!in.Has(moveAt, moveCount * MoveSize)) return fail("the move table runs past the end of the file");
    if (!in.Has(nameAt, moveCount * 4)) return fail("the move names run past the end of the file");

    File file;
    file.charges.resize(chargeCount);
    for (std::size_t i = 0; i < chargeCount; ++i) {
        const std::uint64_t at = chargeAt + i * ChargeSize;
        Charge& charge = file.charges[i];
        charge.input = in.U16(at); charge.forbid = in.U16(at + 2); charge.flags = in.U16(at + 4);
        charge.frames = in.U16(at + 6); charge.grace = in.U16(at + 8); charge.window = in.U16(at + 10);
    }
    file.motions.resize(motionCount);
    for (std::size_t i = 0; i < motionCount; ++i) {
        const std::uint64_t at = motionAt + i * MotionSize;
        const std::uint32_t stepCount = in.U32(at);
        if (stepCount > MaxSteps) return fail("a motion has too many steps");
        file.motions[i].resize(stepCount);
        for (std::size_t j = 0; j < stepCount; ++j) {
            const std::uint64_t from = at + 4 + j * StepSize;
            MotionStep& step = file.motions[i][j];
            step.type = in.U16(from); step.frames = in.U16(from + 2); step.input = in.U16(from + 4);
            step.forbid = in.U16(from + 6); step.match = in.U16(from + 8); step.count = in.U16(from + 10);
            if (step.type > Repeat) return fail("a motion has an unknown step");
        }
    }
    file.moves.resize(moveCount);
    for (std::size_t i = 0; i < moveCount; ++i) {
        const std::uint64_t at = moveAt + i * MoveSize;
        Move& move = file.moves[i];
        move.input = in.U16(at); move.flags = in.U16(at + 2); move.position = in.U16(at + 4); move.rules = in.U16(at + 6);
        move.state = in.U16(at + 0x0A); move.foe = in.U16(at + 0x0C);
        move.stance = static_cast<std::int8_t>(bytes[at + 0x0F]); move.ultra = static_cast<std::int8_t>(bytes[at + 0x10]);
        const std::uint32_t dist = in.U32(at + 0x14);
        std::memcpy(&move.dist, &dist, 4);
        move.meter = static_cast<std::int16_t>(in.U16(at + 0x18)); move.revenge = static_cast<std::int16_t>(in.U16(at + 0x1C));
        move.motionIndex = static_cast<std::int32_t>(in.U32(at + 0x20)); move.script = static_cast<std::int32_t>(in.U32(at + 0x24));
        move.category = in.U32(at + 0x28);
        if (move.motionIndex < -1 || move.motionIndex >= static_cast<std::int32_t>(motionCount)) return fail("a move names a motion the file does not hold");
        if (move.script < -1) return fail("a move names no script");

        const std::uint64_t text = in.U32(nameAt + i * 4);
        std::size_t length = 0;
        while (text + length < size && bytes[text + length]) {
            const std::uint8_t c = bytes[text + length];
            if (c < 0x20 || c > 0x7E || length == MaxName) return fail("a move name is not text");
            ++length;
        }
        if (text + length >= size) return fail("a move name runs past the end of the file");
        move.name.assign(reinterpret_cast<const char*>(bytes + text), length);
        Describe(file, move);
    }
    model = std::move(file);
    return true;
}
} }
