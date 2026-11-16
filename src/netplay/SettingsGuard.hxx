#pragma once
#include <map>
#include <string>

namespace sf4e { namespace netplay {

// The saved settings this run did not read faithfully, so the program's stand-in
// for them (a default) is never written over them. A key held here is left out
// of every save until its value moves from the one it held when it was held:
// the player changing that setting is the one thing that writes it, and a key
// left out keeps what the file has, whatever that is. PlayerPreferencesJson
// applies it (SettingsGuardJson.hxx); the runtime sets it from what a read gave
// (PlayerPreferencesRead.hxx).
struct SettingsGuard {
    // Key to the value the program held for it, as JSON text.
    std::map<std::string, std::string> held;
    // The settings could not be read this run, even tried again: nothing is
    // written, whatever the player changes.
    bool frozen = false;
};

} }
