#pragma once

// The menu's artwork backdrop, as SFV Ember has it: the game's own pictures
// behind every full-window page of Ember's menu, one held for 25 s and then
// cross-faded into the next over 1.2 s, under the page's scrim, so the text
// reads as on Ember's own art. Its rules are common/MenuBackdropRules.hxx;
// the pictures are common/MovieFrames.hxx: frames of the game's prologue and
// opening movies (never an ending) and the game's painted `all_bg` still.
//
// - Where: only with the menu shown over the game's main menu, and no game
//   played, watched or replayed (backdrop::Allowed); never over a fight,
//   Training, a watched game, a replay or the post-match prompt. Elsewhere,
//   with the setting off (Settings > Interface > Look > Background artwork,
//   PlayerPreferences::menuArtwork), or before the first picture has loaded,
//   Ember's own art shows as it always did.
// - Loading, one picture at a time and only while the backdrop was drawn in
//   the last second: the still through SelectionArt, which reads the game's
//   archive on its worker; a movie frame from Ember's cache folder, which
//   Ember's launcher fills on request (platform/MovieFrameMailbox.hxx): the
//   video is never decoded in the game. A frame decoded once is a file for
//   every later session.
// - Memory: at most two pictures alive (1280 pixels wide at most, 3.7 MB
//   each in the managed pool), the one faded out let go at once, both let
//   go 5 s after the backdrop was last drawn, and everything when the
//   setting is turned off. The textures are SelectionArt's, which a device
//   Reset frees with the overlay (ArtGone); the cycle then starts over with
//   the new art, from the cached files.
// - Fallback, never an error the player sees: a frame that fails is skipped;
//   with no launcher, no decoder (Windows N, Wine) or three frames failing
//   in a row the frames are given up for the session and the still holds
//   the screen; with the still failing too, Ember's own art.

#include <string>

namespace sf4e { namespace ui { class SelectionArt; } }
namespace sf4e { namespace backdrop { struct Surface; } }

namespace sf4e { namespace menu_backdrop {

// When the overlay starts (each time): the drawer (ui::SetMenuBackdropDrawer)
// and the cycle, once per session.
void Install();
// The overlay's thread, each frame before the shell draws: where the menu
// is (backdrop::Allowed), whether the player wants the backdrop, the shell's
// screen (its scrim) and the art the pictures load through. Lets go of what
// is no longer shown and moves the load under way on.
void Frame(const backdrop::Surface& surface, bool wanted, const std::string& screen, ui::SelectionArt* art);
// The overlay is freed with its art (a device Reset, the game closing):
// every picture went with the art's textures; the cycle starts over.
void ArtGone();

} }
