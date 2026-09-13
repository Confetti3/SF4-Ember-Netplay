#include "../common/RollbackHud.hxx"
#include <cstdlib>
#include <iostream>
#define CHECK(c) do { if (!(c)) { std::cerr << "Check failed: " #c << '\n'; std::exit(1); } } while (false)
int main() {
    sf4e::RollbackHud hud;
    CHECK(hud.Recent(0) == 0);
    hud.Begin(10);
    for (int i = 0; i < 5; ++i) hud.Replayed(10);
    hud.Begin(500); hud.Replayed(500); hud.Replayed(500);
    CHECK(hud.Recent(1009) == 5);
    CHECK(hud.Recent(1010) == 2);
    CHECK(hud.Recent(1500) == 0);
    hud.Begin(1600); CHECK(hud.Recent(1600) == 0);
    hud.Replayed(1600); hud.Reset(); CHECK(hud.Recent(1600) == 0);
    std::cout << "Rollback depth, expiry, empty loads and session reset passed\n";
}
