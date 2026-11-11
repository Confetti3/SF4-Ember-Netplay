#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace sf4e {

// A player's mark in public rooms (docs/design/SKILL_BADGE.md), as the bridge
// signed it into their room ticket: a rating still placing (shown as "?"), a
// class, or Ember's developer mark in place of a class. A class is one of
// USF4's own online classes, 1 (D) to Classes (Grand Master), unsettled after
// a long break, when it may no longer be right. A default SkillBadge is none:
// no Discord account, not taking part, or no ticket with one.
struct SkillBadge {
	static constexpr std::uint8_t Classes = 10;
	enum class Kind : std::uint8_t { None, Placing, Class, Developer };
	Kind kind = Kind::None;
	// A class's band and whether it is settled; 0 and false for every other kind.
	std::uint8_t band = 0;
	bool settled = false;
	static SkillBadge OfClass(std::uint8_t band, bool settled) { return {Kind::Class, band, settled}; }
	static SkillBadge Placing() { return {Kind::Placing, 0, false}; }
	static SkillBadge Developer() { return {Kind::Developer, 0, false}; }
	bool Present() const { return kind != Kind::None; }
	bool HasClass() const { return kind == Kind::Class; }
	bool Valid() const { return kind == Kind::Class ? band >= 1 && band <= Classes : band == 0 && !settled; }
	bool operator==(const SkillBadge& other) const { return kind == other.kind && band == other.band && settled == other.settled; }
};

// A player's card token (docs/design/FIGHTER_LEVELS.md): the bridge's sealed
// name for their Discord account, signed into their public room ticket, which
// the room shows its members so any of them can ask the bridge for that
// player's card. Opaque here: only its shape is checked, as
// ember_protocol::levels::is_card_token does ("c1." and 1 to MaximumBody
// characters of unpadded base64url). A default CardToken is none.
class CardToken {
public:
	static constexpr std::size_t MaximumBody = 96;
	bool Present() const { return !text_.empty(); }
	const std::string& ToString() const { return text_; }
	// None for text that is not shaped like a token.
	static CardToken Parsed(const std::string& text) {
		static const char prefix[] = "c1.";
		const std::size_t prefixBytes = sizeof(prefix) - 1;
		if (text.size() <= prefixBytes || text.size() > prefixBytes + MaximumBody || text.compare(0, prefixBytes, prefix) != 0) return {};
		for (std::size_t i = prefixBytes; i < text.size(); ++i) {
			const char c = text[i];
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return {};
		}
		CardToken token;
		token.text_ = text;
		return token;
	}
	bool operator==(const CardToken& other) const { return text_ == other.text_; }
	bool operator!=(const CardToken& other) const { return text_ != other.text_; }

private:
	std::string text_;
};

// A game a server-owned room saw won, by its two fighters' accounts: what the
// bridge rates classes and counts fighter levels from. A room keeps its newest
// MaximumWonGames, which the room host reports on every status.
constexpr std::size_t MaximumWonGames = 32;
// The most games before a game in its battle (room::Action::game, WonGame::game).
constexpr std::uint32_t MaximumGameInBattle = 1000;
struct WonGame {
	// No fighter: WonGame::fighters when the room did not know both.
	static constexpr int NoFighter = -1;
	std::uint8_t table = 0;
	std::uint64_t generation = 0;
	std::string winner, loser;
	// The games before this one in its battle: 0 for a battle's first game,
	// more for each rematch played on in place under the same generation.
	std::uint32_t game = 0;
	// The winner's and the loser's fighters (0 to FighterCount - 1), both or
	// neither: NoFighter twice when either was not shared.
	int fighters[2] = {NoFighter, NoFighter};
	bool HasFighters() const { return fighters[0] != NoFighter; }
	bool operator==(const WonGame& other) const {
		return table == other.table && generation == other.generation && winner == other.winner && loser == other.loser &&
			game == other.game && fighters[0] == other.fighters[0] && fighters[1] == other.fighters[1];
	}
};

}
