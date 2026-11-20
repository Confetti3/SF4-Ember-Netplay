#pragma once

// The pictures behind Ember's menu (sf4e__MenuBackdrop.hxx): stills from
// the game's own story movies, chosen by hand, and decoded from the player's
// install at run time, never shipped. Kate's rules (MENU_BACKDROP_DESIGN):
// prologue and opening movies only, never an ending (they show how each
// story ends), and never a video decoder inside the 32-bit game: Ember's
// launcher, which runs beside the game for the whole session, decodes one
// frame at a time into a cache file the game then reads
// (platform/MovieFrameMailbox.hxx). Pure, no Windows: the launcher and the
// game both read the list, and the tests check it.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sf4e { namespace backdrop { class Cycle; } }

namespace sf4e { namespace movie_frames {

// One still: a movie under the game's folder ('/' between names) and the
// time of the frame in it.
struct Frame {
    const char* movie;
    std::uint32_t ms;
};

// The curated list, in the order it was chosen (the cycle shuffles it).
const std::vector<Frame>& Curated();
// The frame of `movie` at `ms` in the list, or -1. The launcher decodes only
// what this finds: anything else in a request is refused.
int Find(std::string_view movie, std::uint32_t ms);

// What a movie is by its folder: a fighter's prologue (movie/prologue/X.wmv,
// or under a dlc/<pack>/ folder), an opening (movie/opening/X.wmv, likewise),
// or anything else: an ending, the credits, a logo clip, a path that is not
// a movie's.
enum class MovieKind { Prologue, Opening, Other };
MovieKind KindOf(std::string_view movie);
// Letters, digits, '_' and '-' in names joined by '/', ending in ".wmv", at
// most kLongestPath characters; no empty name, no "." or ".." name.
constexpr std::size_t kLongestPath = 96;
bool PathSafe(std::string_view movie);
// Every frame a prologue's or an opening's (KindOf), its path safe, its time
// within the first ten minutes and no pair twice; false and why otherwise.
bool ListValid(const std::vector<Frame>& frames, std::string& why);

// The cache file's name for a frame, in Ember's cache folder: the movie's
// path with '/' as '_', without ".wmv", and the time:
// "movie_prologue_RYU_18000.jpg".
std::string FileName(const Frame& frame);

// The backdrop's cycle (MenuBackdropRules.hxx) has the still first, entry
// 0: the game's own painted `all_bg` (resource/ui/main_menu/vfx/
// player_data.emz), read in the game like its other pictures. Then each
// curated frame, entry 1 on.
constexpr int kStillEntry = 0;
int EntryCount();
// The curated frame of a cycle's entry, or -1 for the still (or none).
int FrameOf(int entry);

// The fallback order. While the frames' source holds, the cycle has the
// still and every frame; once it is given up, the frames are dropped from
// the cycle (DropFrames) and the still holds the screen alone; with the
// still failing too the cycle is exhausted and Ember's own art shows, as
// with the setting off. Never an error the player sees. Given up for the
// session when the launcher says no frame can be decoded here, or there is
// no launcher to ask, or after kFailuresInARow frames in a row failed with
// none decoded between them (a decoder that fails each frame without
// saying so, as under Wine without its WMV codec).
class SourceHealth {
public:
    static constexpr int kFailuresInARow = 3;
    void Decoded() { inARow_ = 0; }
    void Failed() { if (++inARow_ >= kFailuresInARow) givenUp_ = true; }
    void Unavailable() { givenUp_ = true; }
    bool GivenUp() const { return givenUp_; }

private:
    int inARow_ = 0;
    bool givenUp_ = false;
};

// A decoded frame is at most this size; the movies are 1280x720.
constexpr int kMostWidth = 1280, kMostHeight = 720;
// The largest picture size that fits `width` by `height` within the most,
// at the same shape (never larger than it was).
void FitWithin(int width, int height, int& fittedWidth, int& fittedHeight);

// Every frame's entry dropped from `cycle` (Cycle::Drop): the source was given up.
void DropFrames(backdrop::Cycle& cycle);

} }
