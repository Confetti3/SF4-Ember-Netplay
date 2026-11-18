#include "../netplay/PlayerPreferencesLoad.hxx"
#include "test_support.hxx"

using Json = nlohmann::json;
using sf4e::netplay::LoadPlayerPreferences;
using sf4e::netplay::PlayerPreferences;

int main() {
    // A profile of readable settings loads without complaint.
    {
        PlayerPreferences value;
        CHECK(LoadPlayerPreferences({{"readySound", false}, {"matchHudSize", 2}, {"readySoundVolume", 50}, {"mainFighter", 5}, {"interfaceScale", 1.5}}, value));
        CHECK(!value.readySound && value.matchHudSize == 2 && value.readySoundVolume == 50 && value.mainFighter == 5 && value.interfaceScale == 1.5f);
    }
    // A setting of the wrong type keeps its own default; every setting after it still loads.
    {
        const PlayerPreferences defaults;
        PlayerPreferences value;
        const Json saved = {{"readySound", 1}, {"discordInvites", false}, {"matchHudSize", "big"}, {"matchHudAnchor", 3},
            {"readySoundVolume", "loud"}, {"mainFighter", 7}, {"interfaceScale", "wide"}};
        CHECK(!LoadPlayerPreferences(saved, value));
        CHECK(value.readySound == defaults.readySound);
        CHECK(!value.discordInvites);
        CHECK(value.matchHudAnchor == 3);
        CHECK(value.readySoundVolume == 100);
        CHECK(value.mainFighter == 7);
        CHECK(value.interfaceScale == defaults.interfaceScale);
    }
    return 0;
}
