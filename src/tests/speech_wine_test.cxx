// Ember's speech under Wine, end to end: the relay starts on the host and
// answers, and Say neither fails nor crashes. Only Wine runs it (elsewhere it
// is skipped); lab/speech-wine-test.sh runs it in the game's Proton prefix on
// a Steam Deck. A wrong calling convention for Wine's own functions crashed
// the game there, which only a run under Wine shows.
#include "../platform/Speech.hxx"
#include "../common/Localization.hxx"
#include <windows.h>
#include <cstdio>

int main() {
    if (!GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) return 77;
    sf4e::loc::SetActive(sf4e::loc::Locale::En);
    sf4e::speech::Say({"Speech test"}, true, 0, 30);
    for (int tries = 0; tries < 40; ++tries) {
        Sleep(500);
        if (sf4e::speech::Unavailable() != sf4e::speech::Missing::Nothing) { std::puts("speech unavailable"); return 1; }
        // The relay reports its voice once it has the request.
        if (sf4e::speech::NaturalVoice().voice != sf4e::speech::Voice::System) { std::puts("speech relay answered"); return 0; }
    }
    std::puts("the relay reported no voice");
    return 1;
}
