#pragma once

// Training loads on the stage TrainingBattle::setupPlayer (0x638570) pushes as
// an immediate (`push 0`, 6A 00) for System vf_248 (the stage) and again for
// getStageBgm (its music); no field holds it. Both pushes change or neither does.
#include "CodeEdit.hxx"

namespace sf4e { namespace training_stage {

// Both pushes hold `from`; afterwards both hold `to`. False, nothing changed,
// for bytes that are not those pushes or a stage that is not a signed byte.
template <class Pages>
bool Push(std::uint8_t* stage, std::uint8_t* music, int from, int to, Pages& pages) {
    if (from < 0 || from > 0x7F || to < 0 || to > 0x7F) return false;
    const auto push = [&](std::uint8_t* at) {
        return code_edit::Edit{ at, 2, { 0x6A, static_cast<std::uint8_t>(from) }, { 0x6A, static_cast<std::uint8_t>(to) } };
    };
    const code_edit::Edit edits[] = { push(stage), push(music) };
    return code_edit::ApplyAll(edits, pages);
}

}}
