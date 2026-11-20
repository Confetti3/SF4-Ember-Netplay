// The menu backdrop's rules (MenuBackdropRules, MovieFrames), checked without
// the game: where it may be drawn; the cycle holds, cross-fades, shuffles
// with no picture twice in a row, loads one at a time, skips a picture that
// does not load, never keeps more than two alive and lets everything go when
// reset; each page gets its scrim and a picture covers any screen at its own
// shape; the fallback order (frames, then the still alone, then Ember's own
// art); and the curated list holds prologue and opening frames only, never
// an ending.
#include "../common/MenuBackdropRules.hxx"
#include "../common/MovieFrames.hxx"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "test_support.hxx"

using namespace sf4e::backdrop;
namespace frames = sf4e::movie_frames;

namespace {

constexpr double kFrame = 1.0 / 60;

bool Near(double a, double b, double within) { return std::fabs(a - b) <= within; }

// The loader and the drawer around a cycle, as sf4e__MenuBackdrop runs
// them: each frame a wanted picture loads at once (or fails where `fails`
// says), the clock moves a frame, and what the cycle lets go is let go.
// Every frame checks that no more than two pictures are alive, on the
// cycle's count and on the loader's, and that nothing is let go that was not
// loaded.
struct Run {
    Cycle cycle;
    std::set<int> alive;
    std::vector<int> shown; // each picture as it became the one shown
    std::function<bool(int)> fails = [](int) { return false; };
    std::vector<int> asked;
    double time = 0;
    explicit Run(int entries, std::uint32_t seed = 7, Timing timing = {}) : cycle(entries, seed, timing) {}
    void Frame(double seconds = kFrame) {
        if (const std::optional<int> wanted = cycle.Wanted()) {
            asked.push_back(*wanted);
            cycle.Started(*wanted);
            EXPECT(!cycle.Wanted().has_value()); // one load at a time
            const bool ok = !fails(*wanted);
            if (cycle.Loaded(*wanted, ok)) alive.insert(*wanted);
            else EXPECT(!ok);
        }
        const int before = cycle.Current();
        cycle.Advance(seconds);
        time += (std::min)(seconds, kMostStep);
        for (int entry : cycle.TakeReleases()) EXPECT(alive.erase(entry) == 1);
        EXPECT(cycle.Alive() <= 2);
        EXPECT(alive.size() <= 2);
        if (cycle.Current() != before && cycle.Current() >= 0) shown.push_back(cycle.Current());
    }
    void For(double seconds) {
        const double until = time + seconds;
        while (time < until) Frame();
    }
    // Frames until `done`, at most `limit` seconds; the time it took.
    double Until(const std::function<bool()>& done, double limit = 120) {
        const double start = time;
        while (!done() && time - start < limit) Frame();
        CHECK(done());
        return time - start;
    }
};

void Gates() {
    Surface menu;
    menu.shellShown = menu.mainMenu = true;
    EXPECT(Allowed(menu));
    EXPECT(!Allowed(Surface{}));
    Surface hidden = menu; hidden.shellShown = false;
    Surface away = menu; away.mainMenu = false; // Training, a practice fight, the post-match prompt
    Surface fighting = menu; fighting.fighting = true;
    Surface watching = menu; watching.watching = true;
    Surface replaying = menu; replaying.replaying = true;
    for (const Surface& refused : {hidden, away, fighting, watching, replaying}) EXPECT(!Allowed(refused));
}

void HoldAndCrossFade() {
    const Timing timing;
    Run run(10, 3, timing);
    EXPECT(run.cycle.Shown().Empty());
    // The first picture fades in from Ember's own art.
    run.Until([&] { return run.cycle.Fading(); }, 1);
    const Cycle::Frame first = run.cycle.Shown();
    EXPECT(first.under.entry < 0 && first.over.entry >= 0 && first.over.alpha < 0.05f);
    const double fadeIn = run.Until([&] { return !run.cycle.Fading(); });
    EXPECT(Near(fadeIn, timing.fade, 0.06));
    CHECK(run.shown.size() == 1);
    const int held = run.cycle.Current();
    EXPECT(run.cycle.Shown().under.entry == held && run.cycle.Shown().under.alpha == 1.f && run.cycle.Shown().over.entry < 0);
    // Held 25 s, the next loaded meanwhile, then the 1.2 s cross-fade.
    const double hold = run.Until([&] { return run.cycle.Fading(); });
    EXPECT(Near(hold, 25, 0.25));
    EXPECT(run.cycle.Shown().under.entry == held && run.cycle.Shown().over.entry == run.cycle.Next());
    EXPECT(run.alive.size() == 2);
    run.For(timing.fade / 2);
    const float middle = run.cycle.Shown().over.alpha;
    EXPECT(middle > 0.3f && middle < 0.7f);
    run.Until([&] { return !run.cycle.Fading(); });
    CHECK(run.shown.size() == 2);
    // The one faded out is let go at once.
    EXPECT(run.shown[1] != held && run.alive.count(held) == 0 && run.alive.size() == 1);
}

void OneLoadAfterTheFadeSettles() {
    Run run(10, 5);
    run.Until([&] { return run.shown.size() == 1; });
    EXPECT(run.asked.size() == 1);
    run.For(Timing{}.settle - 0.1);
    EXPECT(run.asked.size() == 1);
    run.For(0.2);
    EXPECT(run.asked.size() == 2 && run.cycle.NextReady());
    run.For(10);
    EXPECT(run.asked.size() == 2);
}

void ShuffledNeverTwiceInARow() {
    const Timing quick{2, 0.2, 0.1, 0.1};
    for (std::uint32_t seed : {1u, 2u, 99u}) {
        Run run(6, seed, quick);
        run.Until([&] { return run.shown.size() >= 30; }, 600);
        for (size_t i = 1; i < run.shown.size(); i++) EXPECT(run.shown[i] != run.shown[i - 1]);
        const std::set<int> round(run.shown.begin(), run.shown.begin() + 6);
        EXPECT(round.size() == 6);
    }
    Run a(20, 11, quick), b(20, 12, quick);
    a.Until([&] { return a.shown.size() >= 10; }, 600);
    b.Until([&] { return b.shown.size() >= 10; }, 600);
    EXPECT(std::vector<int>(a.shown.begin(), a.shown.begin() + 10) != std::vector<int>(b.shown.begin(), b.shown.begin() + 10));
}

void FailedSkippedAfterAPause() {
    const Timing quick{2, 0.2, 1.0, 0.1};
    Run run(5, 21, quick);
    run.fails = [](int entry) { return entry == 1 || entry == 3; };
    run.Until([&] { return run.shown.size() >= 12; }, 600);
    for (int entry : run.shown) EXPECT(entry != 1 && entry != 3);
    EXPECT(std::count(run.asked.begin(), run.asked.end(), 1) == 1 && std::count(run.asked.begin(), run.asked.end(), 3) == 1);
    EXPECT(run.cycle.FailedCount() == 2 && !run.cycle.Exhausted());
    // The pause after a failure: the next try waits the retry time.
    Cycle cycle(3, 4, quick);
    const int first = *cycle.Wanted();
    cycle.Started(first);
    EXPECT(!cycle.Loaded(first, false));
    EXPECT(!cycle.Wanted().has_value());
    for (int i = 0; i < 3; ++i) cycle.Advance(0.25);
    EXPECT(!cycle.Wanted().has_value());
    cycle.Advance(0.25);
    EXPECT(cycle.Wanted().has_value() && *cycle.Wanted() != first);
}

void AbandonedIsNoFailure() {
    Cycle cycle(3, 9);
    const int first = *cycle.Wanted();
    cycle.Started(first);
    cycle.Abandon(first);
    EXPECT(cycle.FailedCount() == 0 && cycle.Wanted() == first);
}

void ResetLetsEverythingGo() {
    const Timing quick{2, 0.2, 0.1, 0.1};
    Run run(6, 13, quick);
    run.Until([&] { return run.cycle.Current() >= 0 && run.cycle.NextReady(); });
    run.cycle.Reset();
    for (int entry : run.cycle.TakeReleases()) EXPECT(run.alive.erase(entry) == 1);
    EXPECT(run.alive.empty() && run.cycle.Shown().Empty() && run.cycle.Alive() == 0);
    // Back, the next one fades in again from the ground.
    run.Until([&] { return run.cycle.Current() >= 0; });
    // A picture loading when Reset comes is refused when it arrives.
    Cycle cycle(4, 2);
    const int loading = *cycle.Wanted();
    cycle.Started(loading);
    cycle.Reset();
    EXPECT(cycle.Alive() == 1 && !cycle.Loaded(loading, true) && cycle.Alive() == 0);
}

void LongFramesNeverSkipAFade() {
    Run run(4, 17);
    run.Until([&] { return run.cycle.Fading(); }, 1);
    run.cycle.Advance(10); // a hitch
    EXPECT(run.cycle.Fading());
}

void ScrimsAndFit() {
    EXPECT(PageKindOf("home") == PageKind::Column && PageKindOf("online") == PageKind::Column && PageKindOf("training") == PageKind::Column);
    EXPECT(PageKindOf("room") == PageKind::Dense && PageKindOf("tab-interface/look") == PageKind::Dense);
    const Scrim column = ScrimOf(PageKind::Column), dense = ScrimOf(PageKind::Dense);
    // Readable everywhere: never lighter than 60 % ink.
    for (const Scrim& s : {column, dense})
        for (float corner : {s.topLeft, s.topRight, s.bottomRight, s.bottomLeft}) EXPECT(corner >= 0.6f && corner <= 1.f);
    EXPECT(column.topLeft > column.topRight); // left-heavy behind the rows
    const auto same = [](const Scrim& a, const Scrim& b) {
        return Near(a.topLeft, b.topLeft, 1e-5) && Near(a.topRight, b.topRight, 1e-5) && Near(a.bottomRight, b.bottomRight, 1e-5) &&
            Near(a.bottomLeft, b.bottomLeft, 1e-5);
    };
    EXPECT(same(Mix(column, dense, 0), column) && same(Mix(column, dense, 1), dense) && same(Mix(column, dense, 2), dense));
    EXPECT(Near(ScrimAt(column, 0, 0), column.topLeft, 1e-5) && Near(ScrimAt(column, 1, 1), column.bottomRight, 1e-5));
    // A 1280x720 frame fills 16:9 whole; the 2:1 still is cut at the sides.
    const UvRect full = CoverFit(1280, 720, 1920, 1080);
    EXPECT(Near(full.u0, 0, 1e-5) && Near(full.v0, 0, 1e-5) && Near(full.u1, 1, 1e-5) && Near(full.v1, 1, 1e-5));
    const UvRect still = CoverFit(1280, 640, 1920, 1080);
    EXPECT(still.v0 == 0 && Near(still.v1, 1, 1e-5) && still.u0 > 0 && Near(still.u0 + still.u1, 1, 1e-5));
    // A 16:10 screen cuts the frame's sides; an ultra-wide one its top and bottom, a little more of the bottom.
    const UvRect wide = CoverFit(1280, 720, 3440, 1440);
    EXPECT(wide.u0 == 0 && wide.v0 > 0 && 1 - wide.v1 > wide.v0);
    EXPECT(FadeCurve(0) == 0 && FadeCurve(1) == 1 && Near(FadeCurve(.5f), .5, 1e-5) && FadeCurve(2) == 1);
}

void WatchClocks() {
    DrawWatch watch;
    EXPECT(watch.IdleFor(5, 0) && !watch.AskedWithin(1, 0));
    watch.Presented(1, 10);
    watch.Asked();
    EXPECT(watch.AskedWithin(1, 10.5) && !watch.AskedWithin(1, 11.5));
    EXPECT(!watch.IdleFor(5, 14) && watch.IdleFor(5, 15));
    watch.Drawn();
    EXPECT(watch.DrawnLastFrame());
    watch.Presented(2, 10.1);
    EXPECT(watch.DrawnLastFrame());
    watch.Presented(3, 10.2);
    EXPECT(!watch.DrawnLastFrame());
}

// The fallback order: the still and the frames, then the still alone, then
// nothing (Ember's own art).
void FallbackOrder() {
    const Timing quick{2, 0.2, 0.1, 0.1};
    // Three frames failing in a row give the source up; one decoded between resets the count.
    frames::SourceHealth health;
    health.Failed(); health.Failed(); health.Decoded(); health.Failed(); health.Failed();
    EXPECT(!health.GivenUp());
    health.Failed();
    EXPECT(health.GivenUp());
    frames::SourceHealth none;
    none.Unavailable();
    EXPECT(none.GivenUp());
    // Every listed frame in the cycle with the still.
    Run run(frames::EntryCount(), 31, quick);
    EXPECT(run.cycle.Entries() == 1 + static_cast<int>(frames::Curated().size()));
    EXPECT(frames::FrameOf(frames::kStillEntry) == -1 && frames::FrameOf(1) == 0 && frames::FrameOf(frames::EntryCount()) == -1);
    run.Until([&] { return run.shown.size() >= 3; }, 600);
    // Given up: no frame is wanted again, the still comes round and stays.
    frames::DropFrames(run.cycle);
    const std::size_t asked = run.asked.size();
    run.Until([&] { return run.cycle.Current() == frames::kStillEntry; }, 600);
    run.For(120);
    EXPECT(run.cycle.Current() == frames::kStillEntry && !run.cycle.Exhausted());
    for (std::size_t i = asked; i < run.asked.size(); ++i) EXPECT(run.asked[i] == frames::kStillEntry);
    // The still failing too: exhausted, so Ember's own art shows.
    Run bare(frames::EntryCount(), 37, quick);
    frames::DropFrames(bare.cycle);
    bare.fails = [](int entry) { return entry == frames::kStillEntry; };
    bare.For(5);
    EXPECT(bare.cycle.Exhausted() && bare.cycle.Shown().Empty() && !bare.cycle.Wanted().has_value());
    EXPECT(std::count(bare.asked.begin(), bare.asked.end(), frames::kStillEntry) == 1 && bare.asked.size() == 1);
}

// Kate's rule: prologue and opening movies only, never an ending.
void CuratedList() {
    const auto& list = frames::Curated();
    std::string why;
    EXPECT_MSG(frames::ListValid(list, why), "%s", why.c_str());
    EXPECT(list.size() >= 40 && list.size() <= 80);
    std::set<std::string> names;
    for (const auto& frame : list) {
        const std::string movie = frame.movie;
        EXPECT_MSG(movie.find("ending") == std::string::npos && movie.find("credit") == std::string::npos, "%s", frame.movie);
        EXPECT(frames::KindOf(movie) != frames::MovieKind::Other);
        EXPECT(frames::Find(movie, frame.ms) >= 0 && frames::Curated()[static_cast<std::size_t>(frames::Find(movie, frame.ms))].ms == frame.ms);
        names.insert(frames::FileName(frame));
    }
    EXPECT(names.size() == list.size()); // one cache file each
    EXPECT(frames::Find("movie/ending/RYU.wmv", 27000) < 0 && frames::Find("movie/prologue/RYU.wmv", 1) < 0);
    // What a movie is, by its folder.
    EXPECT(frames::KindOf("movie/prologue/RYU.wmv") == frames::MovieKind::Prologue);
    EXPECT(frames::KindOf("dlc/04_ae2/movie/prologue/DCP.wmv") == frames::MovieKind::Prologue);
    EXPECT(frames::KindOf("movie/opening/opening_r.wmv") == frames::MovieKind::Opening);
    EXPECT(frames::KindOf("dlc/04_ae2/movie/opening/opening.wmv") == frames::MovieKind::Opening);
    for (const char* refused : {"movie/ending/RYU.wmv", "dlc/03_character/movie/ending/YUN.wmv", "movie/credit/ending.wmv",
             "movie/nvidia/TheWay_1280x800.wmv", "movie/prologue/../ending/RYU.wmv", "movie/prologue/RYU.mp4", "/movie/prologue/RYU.wmv",
             "movie//prologue/RYU.wmv", "C:/movie/prologue/RYU.wmv", "movie/prologue/RYU.wmv/x", "dlc/x/y/movie/prologue/RYU.wmv", ""})
        EXPECT_MSG(frames::KindOf(refused) == frames::MovieKind::Other, "%s", refused);
    // A list with an ending, a twice-listed frame or a time past ten minutes is refused.
    EXPECT(!frames::ListValid({{"movie/prologue/RYU.wmv", 1000}, {"movie/ending/RYU.wmv", 1000}}, why));
    EXPECT(!frames::ListValid({{"movie/prologue/RYU.wmv", 1000}, {"movie/prologue/RYU.wmv", 1000}}, why));
    EXPECT(!frames::ListValid({{"movie/prologue/RYU.wmv", 600001}}, why) && !frames::ListValid({}, why));
    EXPECT(frames::FileName({"movie/prologue/RYU.wmv", 18000}) == "movie_prologue_RYU_18000.jpg");
    EXPECT(frames::FileName({"dlc/04_ae2/movie/opening/opening.wmv", 12800}) == "dlc_04_ae2_movie_opening_opening_12800.jpg");
    // Decoded frames fit 1280x720 at their own shape.
    int w = 0, h = 0;
    frames::FitWithin(1280, 720, w, h); EXPECT(w == 1280 && h == 720);
    frames::FitWithin(1920, 1080, w, h); EXPECT(w == 1280 && h == 720);
    frames::FitWithin(1280, 800, w, h); EXPECT(w == 1152 && h == 720);
    frames::FitWithin(600, 336, w, h); EXPECT(w == 600 && h == 336);
}

} // namespace

int main() {
    Gates();
    HoldAndCrossFade();
    OneLoadAfterTheFadeSettles();
    ShuffledNeverTwiceInARow();
    FailedSkippedAfterAPause();
    AbandonedIsNoFailure();
    ResetLetsEverythingGo();
    LongFramesNeverSkipAFade();
    ScrimsAndFit();
    WatchClocks();
    FallbackOrder();
    CuratedList();
    return TEST_EXIT_CODE();
}
