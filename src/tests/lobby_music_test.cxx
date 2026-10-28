#include "../common/LobbyMusic.hxx"
#include "../sf4e/sf4e__MenuMusic.hxx"
#include <cstring>
#include <random>
#include <set>
#include <string>

#include "test_support.hxx"

using namespace sf4e;

namespace {
const char native[] = "ui/sound/bgm/BGM_MAIN.csb";

// A stand-in for the game's image: the menu's path lies in it, and the entry points at that path.
struct Game {
    char image[64] = {};
    const char* entry = nullptr;
    char elsewhere[sizeof(native)] = {};
    explicit Game(const char* text = native) {
        std::strcpy(image + 8, text);
        entry = image + 8;
    }
    MenuMusic::Target Target() {
        return {&entry, native, reinterpret_cast<std::uintptr_t>(image), reinterpret_cast<std::uintptr_t>(image) + sizeof(image)};
    }
};

void Picks() {
    using lobby_music::Path;
    CHECK(lobby_music::LobbyStageCount == 21);
    CHECK(lobby_music::RollCount == 21 + selection::FighterCount && lobby_music::RollCount == 65);
    std::set<std::string> seen;
    for (unsigned roll = 0; roll < lobby_music::RollCount; ++roll) {
        const std::string path = Path(roll);
        const std::string folder = "battle/sound/bgm/BGM_";
        CHECK(path.compare(0, folder.size(), folder) == 0);
        CHECK(path.size() > folder.size() + 4 && path.compare(path.size() - 4, 4, ".csb") == 0);
        CHECK(seen.insert(path).second);
        if (roll < lobby_music::LobbyStageCount) {
            const auto* stage = selection::FindStage(1 + roll);
            CHECK(stage && path == folder + stage->code + ".csb");
        } else {
            const auto* fighter = selection::FindFighter(static_cast<int>(roll - lobby_music::LobbyStageCount));
            CHECK(fighter && path == folder + fighter->code + "_2CH.csb");
        }
    }
    // The first and last stage of the lobby's pool, and the first and last fighter.
    CHECK(Path(0) == "battle/sound/bgm/BGM_CHN.csb" && Path(20) == "battle/sound/bgm/BGM_LBX.csb");
    CHECK(Path(21) == "battle/sound/bgm/BGM_RYU_2CH.csb");
    CHECK(Path(lobby_music::RollCount - 1) == std::string("battle/sound/bgm/BGM_") + selection::FindFighter(selection::FighterCount - 1)->code + "_2CH.csb");
    // Every fighter has a roll, not only the first 35; no stage past the pool does.
    for (int fighter = 0; fighter < selection::FighterCount; ++fighter)
        CHECK(seen.count(std::string("battle/sound/bgm/BGM_") + selection::FindFighter(fighter)->code + "_2CH.csb") == 1);
    for (const auto& stage : selection::StageList())
        CHECK(seen.count("battle/sound/bgm/BGM_" + std::string(stage.code) + ".csb") == (stage.id >= 1 && stage.id <= 21 ? 1u : 0u));
    // A roll past the end wraps.
    CHECK(Path(lobby_music::RollCount) == Path(0) && Path(lobby_music::RollCount + 30) == Path(30));
    CHECK(Path(~0u) == Path(~0u % lobby_music::RollCount));
}

void Initialize() {
    Game game;
    const char* const original = game.entry;
    MenuMusic::Entry entry(game.Target());
    CHECK(!entry.Unavailable());
    CHECK(game.entry == original);

    // Not the menu track, or not in the image: refused, and never written.
    const auto refused = [](Game& other, MenuMusic::Target target) {
        const char* const before = other.entry;
        MenuMusic::Entry untouched(target);
        CHECK(untouched.Unavailable());
        untouched.Use(true, 3);
        untouched.Pick(4);
        untouched.Use(false, 5);
        CHECK(other.entry == before);
    };
    Game wrongText("ui/sound/bgm/BGM_XXXX.csb");
    refused(wrongText, wrongText.Target());
    Game prefix("ui/sound/bgm/BGM_MAIN.cs");
    refused(prefix, prefix.Target());
    Game outside;
    std::strcpy(outside.elsewhere, native);
    outside.entry = outside.elsewhere;
    refused(outside, outside.Target());
    Game cutOff;
    auto target = cutOff.Target();
    target.imageEnd = reinterpret_cast<std::uintptr_t>(cutOff.entry) + sizeof(native) - 1;
    refused(cutOff, target);
    Game before;
    target = before.Target();
    target.imageBegin = reinterpret_cast<std::uintptr_t>(before.entry) + 1;
    refused(before, target);
    Game unwritable;
    target = unwritable.Target();
    target.entry = nullptr;
    MenuMusic::Entry noEntry(target);
    CHECK(noEntry.Unavailable());
    noEntry.Use(true, 1);
}

void EnableAndDisable() {
    Game game;
    const char* const original = game.entry;
    MenuMusic::Entry entry(game.Target());
    // Off to begin with: nothing is written, whatever is picked.
    entry.Use(false, 7);
    entry.Pick(8);
    CHECK(game.entry == original);

    entry.Use(true, 7);
    CHECK(game.entry != original && lobby_music::Path(7) == game.entry);
    const char* const seven = game.entry;
    // Repeated enable keeps the theme that plays.
    entry.Use(true, 9);
    CHECK(game.entry == seven);
    // Another theme, then the first again at the very same string.
    entry.Pick(9);
    CHECK(lobby_music::Path(9) == game.entry && game.entry != seven);
    const char* const nine = game.entry;
    entry.Pick(7);
    CHECK(game.entry == seven);
    entry.Pick(9 + lobby_music::RollCount);
    CHECK(game.entry == nine);

    // Off puts the game's own pointer back, and picks leave it there.
    entry.Use(false, 1);
    CHECK(game.entry == original && std::strcmp(game.entry, native) == 0);
    entry.Pick(2);
    entry.Use(false, 3);
    CHECK(game.entry == original);

    // On again picks anew; the themes are the same strings as before.
    entry.Use(true, 7);
    CHECK(game.entry == seven);
    entry.Use(false, 0);
    CHECK(game.entry == original);
}

void EveryRoll() {
    Game game;
    MenuMusic::Entry entry(game.Target());
    entry.Use(true, 0);
    // A seeded source stands in for the game's: every pick is a theme of the lobby.
    std::mt19937 rolls(20261028);
    std::set<std::string> played;
    for (int pick = 0; pick < 2000; ++pick) {
        const unsigned roll = static_cast<unsigned>(rolls());
        entry.Pick(roll);
        CHECK(lobby_music::Path(roll) == game.entry);
        played.insert(game.entry);
    }
    CHECK(played.size() == lobby_music::RollCount);
}
}

int main() {
    Picks();
    Initialize();
    EnableAndDisable();
    EveryRoll();
    return TEST_EXIT_CODE();
}
