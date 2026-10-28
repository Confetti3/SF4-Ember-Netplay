#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "../common/LobbyMusic.hxx"

// Endless lobby music (Interface > Endless lobby music, on by default): the main menu
// plays a stage or character theme of the game's lobby instead of looping its menu
// track, and another one after every match. The menu reads its music's path from a
// table entry each time it starts its music, so the entry is pointed at a theme, or
// back at the game's own path. Nothing on disk changes, and nothing runs per frame.
// Everything here runs on the game thread: Use at start-up and when the setting is
// saved (NetplayRuntime), Pick as a battle closes (fSystem::CloseBattle).
namespace sf4e {
	namespace MenuMusic {
		// The menu's table entry and what must be true of it before it is written.
		struct Target {
			// The entry, or null where it is not writable data of the game.
			const char** entry;
			// The path it holds in the game, which the entry has to point at.
			const char* nativePath;
			// The game's image, which the entry's pointer has to lie in.
			std::uintptr_t imageBegin, imageEnd;
		};

		// The entry, checked once when it is made. An entry that is not the one this
		// code knows is never written. Themes are fixed strings, one per roll, so the
		// entry never points at text that is rewritten in place.
		class Entry {
		public:
			explicit Entry(const Target& target) : target_(target) {
				unavailable_ = Check();
				if (unavailable_) return;
				original_ = *target_.entry;
				for (unsigned roll = 0; roll < lobby_music::RollCount; ++roll) themes_.push_back(lobby_music::Path(roll));
			}
			Entry(const Entry&) = delete;
			Entry& operator=(const Entry&) = delete;
			// Why the entry is left alone, or null when it is used.
			const char* Unavailable() const { return unavailable_; }
			// On: a theme for the roll. Off: the game's own pointer back. Asking for
			// the state it is in changes nothing.
			void Use(bool endless, unsigned roll) {
				if (unavailable_ || endless == on_) return;
				on_ = endless;
				if (on_) Pick(roll);
				else *target_.entry = original_;
			}
			// With the setting on, another theme.
			void Pick(unsigned roll) {
				if (!unavailable_ && on_) *target_.entry = themes_[roll % lobby_music::RollCount].c_str();
			}
		private:
			const char* Check() const {
				if (!target_.entry) return "its table entry is not writable game data";
				const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(*target_.entry);
				const std::size_t size = std::strlen(target_.nativePath) + 1;
				if (at < target_.imageBegin || at >= target_.imageEnd || target_.imageEnd - at < size)
					return "its table entry points outside the game";
				if (std::memcmp(*target_.entry, target_.nativePath, size) != 0) return "its table entry is not the menu track";
				return nullptr;
			}
			Target target_;
			const char* unavailable_ = nullptr;
			const char* original_ = nullptr;
			bool on_ = false;
			std::vector<std::string> themes_;
		};

		// The saved setting, applied (the first call checks the game's entry).
		void Use(bool endless);
		// As a battle closes: with the setting on, another theme.
		void Pick();
		// Whether the game's entry is the supported one, so the setting does something.
		bool Available();
	}
}
