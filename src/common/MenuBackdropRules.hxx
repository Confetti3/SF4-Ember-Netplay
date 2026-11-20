#pragma once

// The rules of the menu's artwork backdrop, with no game, no Direct3D and no
// ImGui, so the tests check all of them. Ported from SFV Ember's
// SfvBackdropRules (its art set aside: SF4's pictures are MovieFrames.hxx):
// - where it may be drawn: the shell shown over the game's main menu, with no
//   game played, watched or replayed;
// - the cycle: a shuffled order with no picture twice in a row, a picture
//   held for a while and then cross-faded into the next, one load at a time,
//   a picture that does not load skipped for the session, and the clock
//   running only while the backdrop is on screen;
// - the scrim each page gets: light and left-heavy behind a narrow column
//   (Home), heavier and even behind a dense page;
// - the cover fit of a picture to the screen.
// sf4e__MenuBackdrop runs them: it loads through SelectionArt and draws
// behind the shell's pages (ui::SetMenuBackdropDrawer).

#include <cstdint>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

namespace sf4e { namespace backdrop {

// --- Where the backdrop may be drawn ---

// What the shell is over in a frame. The backdrop is for the game's main menu
// and nothing that plays: a game being prepared or played (Training
// included), a watched game and a replay (or its export) are not it, and
// neither is the post-match prompt over a game's result.
struct Surface {
    bool shellShown = false;
    bool mainMenu = false;
    bool fighting = false;  // a game is being prepared or played, or Training runs
    bool watching = false;  // a watched game is on
    bool replaying = false; // a replay plays back or is exported
};
bool Allowed(const Surface& surface);

// --- The page's scrim ---

// The kind of page the backdrop is behind: a narrow column of rows with the
// screen open beside it, or a page that fills the screen (boards, tables,
// settings), which is every page not named a column.
enum class PageKind { Column, Dense };
// By the shell's screen ("home", "online", "training" are columns).
PageKind PageKindOf(std::string_view screen);

// The theme's ink over the picture, 0 to 1 at each corner.
struct Scrim {
    float topLeft = 1, topRight = 1, bottomRight = 1, bottomLeft = 1;
    bool operator==(const Scrim& o) const {
        return topLeft == o.topLeft && topRight == o.topRight && bottomRight == o.bottomRight && bottomLeft == o.bottomLeft;
    }
    bool operator!=(const Scrim& o) const { return !(*this == o); }
};
Scrim ScrimOf(PageKind kind);
// From `from` to `to` by `t` (0 to 1, clamped), each corner alike.
Scrim Mix(const Scrim& from, const Scrim& to, float t);
// The scrim at a point of the screen (`x` and `y` from 0 at the left and top
// to 1), between its corners as the drawn gradient blends them.
float ScrimAt(const Scrim& scrim, float x, float y);

// The pictures are drawn through a warm grey (a multiply), so a cold picture
// sits on the theme's gold-on-ink ramp with the rest of the page.
struct Tint {
    unsigned char r, g, b;
};
constexpr Tint kArtTint{240, 232, 216};
// How long a change of page kind takes the scrim from one to the other.
constexpr double kScrimSeconds = 0.35;

// --- Cover fit ---

// The part of a `width` by `height` picture (texture coordinates, 0 to 1)
// that fills a `targetWidth` by `targetHeight` rect at the picture's own
// shape: all of the picture's height and its centre's width on a narrower
// screen, all of its width on a wider one, the cut placed by `anchorX` and
// `anchorY` (0 keeps the left or top edge, 1 the right or bottom). The whole
// picture for a size that is not positive.
struct UvRect {
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
};
constexpr float kAnchorX = 0.5f, kAnchorY = 0.4f;
UvRect CoverFit(int width, int height, float targetWidth, float targetHeight, float anchorX = kAnchorX, float anchorY = kAnchorY);

// --- The cycle ---

struct Timing {
    double hold = 25;    // a picture's time on screen, once faded in
    double fade = 1.2;   // the cross-fade
    double retry = 1.0;  // after a picture that did not load, before the next is tried
    double settle = 2.0; // after a fade, before the next picture is loaded
};
// The most a frame may move the cycle's clock: a long frame (a hitch, the
// window away) never skips a fade.
constexpr double kMostStep = 0.25;
// The fade's curve, 0 to 1: smoothstep.
float FadeCurve(float progress);

// Which picture shows, which loads next and when they change. Entries are
// indexes 0 to entries - 1. Not thread-safe.
class Cycle {
public:
    Cycle() = default;
    Cycle(int entries, std::uint32_t seed, Timing timing = {});

    // The entry the loader should load now; none while it loads one, while
    // two are alive, during a fade or its settling time, or after a failure
    // until the retry time has passed.
    std::optional<int> Wanted() const;
    // The loader took the wanted entry.
    void Started(int entry);
    // The loader's answer for `entry`: loaded, or not (skipped for the rest
    // of the session). True when the cycle keeps the picture; false when it
    // does not want it any more (Reset since), and the loader lets it go.
    bool Loaded(int entry, bool ok);
    // The loader let `entry` go before it arrived: not a failure. The next
    // one is wanted again, and stays eligible.
    void Abandon(int entry);
    // `entry` will never load this session (its source is gone, such as a
    // frame decoder that is not there): skipped as a failure is, without a
    // load. A picture already shown or ready stays until it is faded out.
    void Drop(int entry);
    // The backdrop was on screen for `seconds` (clamped to kMostStep).
    void Advance(double seconds);
    // Fade to the next picture as soon as it has loaded, whatever the hold.
    void Step();
    // Everything let go (the setting turned off, the backdrop idle, the
    // device's pictures gone): what was alive goes to TakeReleases, an entry
    // still loading is refused when it arrives, and the order goes on where
    // it was.
    void Reset();

    // What to draw: the picture under (alpha 1, or none) and the one fading
    // in over it (alpha 0 to 1, or none).
    struct Layer {
        int entry = -1;
        float alpha = 0;
    };
    struct Frame {
        Layer under, over;
        bool Empty() const { return under.entry < 0 && over.entry < 0; }
    };
    Frame Shown() const;
    // Entries whose pictures to let go now, once each.
    std::vector<int> TakeReleases();

    // The entries loading or loaded, a refused one not yet arrived included.
    int Alive() const;
    // Every entry failed or dropped (or there are none): nothing will ever show.
    bool Exhausted() const;
    bool Failed(int entry) const { return entry >= 0 && entry < entries_ && failed_[(size_t)entry]; }
    int Current() const { return current_.ready ? current_.entry : -1; }
    int Next() const { return next_.entry; }
    bool NextReady() const { return next_.ready; }
    bool Fading() const { return fading_; }
    double Held() const { return held_; }
    int FailedCount() const { return failedCount_; }
    int Entries() const { return entries_; }

private:
    struct Slot {
        int entry = -1;
        bool loading = false, ready = false;
    };
    int Pick();
    // A next picture chosen when there is none, outside a fade.
    void Refill();
    void StartFade();
    void Skip(int entry);

    int entries_ = 0;
    Timing timing_;
    std::mt19937 random_;
    std::vector<int> order_;
    size_t cursor_ = 0;
    int lastPicked_ = -1;
    std::vector<bool> failed_;
    int failedCount_ = 0;
    Slot current_, next_;
    int stale_ = -1; // loading when Reset came: refused when it arrives, and alive until then
    bool fading_ = false, stepWanted_ = false;
    double clock_ = 0, faded_ = 0, held_ = 0, settleUntil_ = 0, retryAt_ = 0;
    std::vector<int> releases_;
};

// When the backdrop was last asked for and last drawn, on two clocks the
// cycle's own does not stop: the frames the overlay draws and the wall clock.
// Asked is the drawer called while the shell allows the backdrop, whether or
// not it had a picture to draw; drawn is it drawing one. Not thread-safe.
class DrawWatch {
public:
    // Each overlay frame, before the shell draws: its number (rising) and the
    // wall clock in seconds.
    void Presented(std::uint64_t frame, double now);
    void Asked();
    void Drawn();
    // Drawn in the frame being drawn or in the one just presented.
    bool DrawnLastFrame() const;
    // Asked within `seconds` before `now` on the wall clock (the loader's gate).
    bool AskedWithin(double seconds, double now) const;
    // Neither asked for nor drawn for at least `seconds` before `now`, or never
    // asked for (a draw is always asked for first).
    bool IdleFor(double seconds, double now) const;

private:
    std::uint64_t frame_ = 0, drawnFrame_ = 0;
    double now_ = 0, askedAt_ = 0;
    bool asked_ = false, drawn_ = false;
};
// The backdrop asked for this long ago still lets the loader start a load.
constexpr double kAskedSeconds = 1.0;
// Not asked for this long (the shell closed, a fight), both pictures are let
// go; the next one fades in from Ember's own art when the backdrop is back.
constexpr double kIdleReleaseSeconds = 5.0;

} }
