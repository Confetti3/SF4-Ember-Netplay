#pragma once
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

// The in-game themes' crash guard: community files are used as they are, and a
// battle HUD that the game cannot load closes the game. So Ember writes down which
// theme it is about to have the game load (armed), and writes down that it loaded
// (proven) once the fight has started. A game that closes in between leaves the
// mark behind, and at the next start that theme is turned off for that part
// (closed), instead of closing the game again at every battle.
// Parts are named by their folder (game_themes::Parts). The state is a plain
// value; Load and Save keep it in <game>\ember\theme-guard.json.
namespace sf4e { namespace platform { namespace game_themes {
constexpr wchar_t GuardFile[] = L"ember\\theme-guard.json";

struct Guard {
    std::map<std::string, std::string> armed;                // part -> the theme being loaded now
    std::map<std::string, std::string> proven;               // part -> the theme that has loaded
    std::map<std::string, std::set<std::string>> closed;     // part -> themes the game closed on
};
struct TurnedOff { std::string part, theme; };

// Each returns whether the state changed, so a caller saves only then.
// Arm: the game is about to load the theme's files. Nothing for the game's own
// (empty name), for a theme that has loaded since it was picked, or one already armed.
bool Arm(Guard& guard, const std::string& part, const std::string& theme);
// The game loaded the theme's files: it is proven, and no longer closed.
bool Prove(Guard& guard, const std::string& part, const std::string& theme);
// The game closed the battle that was loading the theme without reaching the fight: nothing is blamed on it.
bool Disarm(Guard& guard, const std::string& part, const std::string& theme);
// The files of the part changed (another pick, or edits written over it): it has to prove itself again.
bool Forget(Guard& guard, const std::string& part);
// At start-up: every theme still armed is the one the game closed on. It becomes
// closed and unarmed, and is returned.
std::vector<TurnedOff> Recover(Guard& guard);
bool Closed(const Guard& guard, const std::string& part, const std::string& theme);

// The state as text and back. Text that is empty, cut short or not the state is no state.
std::string ToJson(const Guard& guard);
Guard FromJson(const std::string& text);
// A missing or unreadable file is no state; Save says whether the file is written (durably, replacing).
Guard Load(const std::filesystem::path& gameDirectory);
bool Save(const std::filesystem::path& gameDirectory, const Guard& guard);
} } }
