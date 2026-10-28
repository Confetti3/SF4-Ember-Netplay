#include "../common/TrainingStage.hxx"
#include <cstdio>

namespace {
struct Pages {
    int refuse = -1, unlocks = 0, open = 0;
    bool Unlock(std::uint8_t*, std::uint8_t, unsigned long&) { if (unlocks == refuse) return false; ++unlocks; ++open; return true; }
    void Lock(std::uint8_t*, std::uint8_t, unsigned long) { --open; }
};
}

int main() {
    int failures = 0;
    const auto check = [&](bool value, const char* message) { if (!value) { ++failures; std::printf("FAIL: %s\n", message); } };
    using sf4e::training_stage::Push;
    std::uint8_t stage[2] = { 0x6A, 0 }, music[2] = { 0x6A, 0 };
    Pages pages;
    check(Push(stage, music, 0, 9, pages) && stage[1] == 9 && music[1] == 9 && pages.open == 0, "both pushes did not take the stage");
    check(!Push(stage, music, 0, 11, pages) && stage[1] == 9 && music[1] == 9, "pushes holding another stage were written");
    check(Push(stage, music, 9, 0, pages) && stage[1] == 0 && music[1] == 0, "the Training stage did not come back");
    check(!Push(stage, music, 0, 0x80, pages) && stage[1] == 0, "a stage past a signed byte was written");
    Pages refusing; refusing.refuse = 1;
    check(!Push(stage, music, 0, 5, refusing) && stage[1] == 0 && music[1] == 0 && refusing.open == 0, "one push changed without the other");
    std::uint8_t other[2] = { 0x53, 0xE8 };
    check(!Push(other, music, 0, 5, pages) && other[0] == 0x53 && other[1] == 0xE8 && music[1] == 0, "another game build's bytes were written");
    return failures ? 1 : 0;
}
