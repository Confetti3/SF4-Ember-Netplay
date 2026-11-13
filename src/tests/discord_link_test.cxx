// Whether a Discord account is linked (netplay/DiscordLink.hxx): the one rule every screen that asks reads.
#include "../netplay/DiscordLink.hxx"

#include <cstdio>

#include "test_support.hxx"

using sf4e::netplay::DiscordLink;
using sf4e::netplay::DiscordLinkReads;
using sf4e::netplay::LinkOf;
using State = DiscordLink::State;

int main() {
	// Nothing read: nothing is known, and nothing is claimed.
	EXPECT(LinkOf({}).state == State::Unknown);
	// No Ember ID to play under: none, whatever else says.
	DiscordLinkReads noId;
	noId.noEmberId = true; noId.standing = true; noId.account = std::string("Player 1");
	EXPECT(LinkOf(noId).state == State::None);
	// The Ember ID screens' read decides, by name, over every other read.
	DiscordLinkReads read;
	read.account = std::string("Player 1"); read.standing = false; read.points = false;
	const auto named = LinkOf(read);
	EXPECT(named.state == State::Linked && named.name == "Player 1");
	read.account = std::string(); read.standing = true; read.friends = true;
	EXPECT(LinkOf(read).state == State::None && LinkOf(read).name.empty());
	// Without it, any read that finds an account wins over one that found none (those go stale once one is linked).
	for (int which = 0; which < 3; ++which) {
		DiscordLinkReads mixed;
		mixed.standing = which == 0; mixed.points = which == 1; mixed.friends = which == 2;
		EXPECT(LinkOf(mixed).state == State::Linked && LinkOf(mixed).name.empty());
	}
	DiscordLinkReads none;
	none.points = false;
	EXPECT(LinkOf(none).state == State::None);
	none.points.reset(); none.friends = false;
	EXPECT(LinkOf(none).state == State::None);
	if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
	else std::printf("discord link tests passed\n");
	return TEST_EXIT_CODE();
}
