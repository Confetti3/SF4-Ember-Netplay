// The desync check of a game with no battle state to compare: the room
// client's verifier for the runtime's tests under fake_game.cxx, which take
// the verification messages and compare nothing. The room client still
// buffers the remote hashes; this verifier never erases them.
#include "../session/BattleVerifier.hxx"

namespace {
class FakeVerifier final : public sf4e::session::BattleVerifier {
public:
	void OnRemoteSnapshot(sf4e::SessionClient&, const sf4e::SessionProtocol::StateSnapshot&) override {}
	void Step(sf4e::SessionClient&, std::map<int, sf4e::SessionProtocol::BattleHashV2>&) override {}
	void Reset() override {}
};
}

std::unique_ptr<sf4e::session::BattleVerifier> sf4e::session::MakeBattleVerifier() {
	return std::make_unique<FakeVerifier>();
}
