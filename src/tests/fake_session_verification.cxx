// The room client's state verification for a game with no battle state to
// compare. SF4's (session/sf4e__SessionClient__Verification.cxx) reads SF4's
// battle system, which the runtime's tests under fake_game.cxx do not link;
// the verification moves behind the game with the session wire (step 6 of
// EMBER_LIBRARY.md). Until then these take the verification messages and
// compare nothing.
#include "../session/sf4e__SessionClient.hxx"

namespace sf4e {
void SessionClient::TerminateOnDesync(const char*, int) {}
bool SessionClient::HandleBattleSnapshot(nlohmann::json&) { return true; }
bool SessionClient::HandleBattleHash(nlohmann::json&) { return true; }
void SessionClient::StepBattleVerification() {}
}
