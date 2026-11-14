#include "../platform/GameThemeGuard.hxx"
#include "test_support.hxx"
#include <filesystem>
#include <fstream>
#include <string>
#include <windows.h>

namespace fs = std::filesystem;
using namespace sf4e::platform::game_themes;

namespace {
const std::string Hud = "battle\hud", Menu = "ui\main_menu";

struct Folder {
    fs::path root;
    Folder() {
        wchar_t temp[MAX_PATH], unique[MAX_PATH];
        CHECK(GetTempPathW(MAX_PATH, temp));
        CHECK(GetTempFileNameW(temp, L"s4g", 0, unique));
        CHECK(DeleteFileW(unique));
        root = unique;
        CHECK(fs::create_directory(root));
    }
    ~Folder() { std::error_code ignored; fs::remove_all(root, ignored); }
};

// The game loads a theme and closes: the next start finds it armed.
void Sequence() {
    Guard guard;
    // The game's own is never armed; a new theme is, once.
    CHECK(!Arm(guard, Hud, ""));
    CHECK(Arm(guard, Hud, "Neon") && !Arm(guard, Hud, "Neon"));
    // The fight is reached: proven, nothing armed, so the next load needs no mark.
    CHECK(Prove(guard, Hud, "Neon") && guard.armed.empty());
    CHECK(!Arm(guard, Hud, "Neon") && !Prove(guard, Hud, "Neon"));
    CHECK(Recover(guard).empty() && !Closed(guard, Hud, "Neon"));

    // Another pick starts over: it is armed, and Neon is no longer proven.
    CHECK(Arm(guard, Hud, "Blue"));
    CHECK(Prove(guard, Hud, "Blue"));
    CHECK(Arm(guard, Hud, "Neon"));
    CHECK(Prove(guard, Hud, "Neon") && guard.proven[Hud] == "Neon");

    // A part whose files changed has to prove itself again.
    CHECK(Forget(guard, Hud) && !Forget(guard, Hud));
    CHECK(Arm(guard, Hud, "Neon"));

    // The game closed while it loaded: found armed at start-up, turned off, marked closed, unarmed.
    const auto turned = Recover(guard);
    CHECK(turned.size() == 1 && turned[0].part == Hud && turned[0].theme == "Neon");
    CHECK(guard.armed.empty() && Closed(guard, Hud, "Neon") && !Closed(guard, Hud, "Blue") && !Closed(guard, Menu, "Neon"));
    CHECK(Recover(guard).empty());

    // Picking it again arms the guard again; it stays marked until it has loaded.
    CHECK(Arm(guard, Hud, "Neon") && Closed(guard, Hud, "Neon"));
    CHECK(Prove(guard, Hud, "Neon") && !Closed(guard, Hud, "Neon") && guard.closed.empty());

    // Closing on a theme that was proven before drops the proof.
    CHECK(Forget(guard, Hud) && Prove(guard, Hud, "Blue") && Arm(guard, Hud, "Neon"));
    CHECK(Recover(guard).size() == 1 && Closed(guard, Hud, "Neon") && guard.proven[Hud] == "Blue");
}

void RoundTrip() {
    Guard guard;
    Arm(guard, Hud, "N\xC3\xA9" "on");
    Prove(guard, Menu, "Blue");
    guard.closed[Hud] = { "Old", "Older" };
    const Guard back = FromJson(ToJson(guard));
    CHECK(back.armed == guard.armed && back.proven == guard.proven && back.closed == guard.closed);

    // Text that is not the state is no state, never a failure.
    for (const char* text : { "", "{", "[1,2]", "null", "{\"armed\":5,\"proven\":[1],\"closed\":{\"a\":3,\"b\":[7,\"x\",\"\"]}}", "\xFF\xFE" }) {
        const Guard none = FromJson(text);
        CHECK(none.armed.empty() && none.proven.empty());
    }
    const Guard partial = FromJson("{\"armed\":{\"battle\\hud\":\"A\",\"x\":4},\"closed\":{\"b\":[7,\"x\",\"\"]}}");
    CHECK(partial.armed.size() == 1 && partial.armed.at(Hud) == "A" && partial.closed.size() == 1 && partial.closed.at("b").size() == 1);
}

void Files() {
    Folder folder;
    CHECK(Load(folder.root).armed.empty());          // no file
    Guard guard;
    Arm(guard, Hud, "Neon");
    CHECK(Save(folder.root, guard));                 // also makes the ember folder
    CHECK(fs::is_regular_file(folder.root / GuardFile));
    CHECK(Load(folder.root).armed == guard.armed);
    Prove(guard, Hud, "Neon");
    CHECK(Save(folder.root, guard) && Load(folder.root).proven == guard.proven && Load(folder.root).armed.empty());
    // The mark survives the game closing: loaded again, it is what Recover finds.
    Arm(guard, Hud, "Blue");
    CHECK(Save(folder.root, guard));
    Guard restarted = Load(folder.root);
    CHECK(Recover(restarted).size() == 1 && Closed(restarted, Hud, "Blue"));

    // An empty or damaged file is no state.
    { std::ofstream(folder.root / GuardFile, std::ios::binary | std::ios::trunc); }
    CHECK(Load(folder.root).armed.empty());
    { std::ofstream(folder.root / GuardFile, std::ios::binary | std::ios::trunc) << "{\"armed\": {\"battle"; }
    CHECK(Load(folder.root).armed.empty() && Load(folder.root).proven.empty());
}
}

int main() {
    Sequence();
    RoundTrip();
    Files();
    return 0;
}
