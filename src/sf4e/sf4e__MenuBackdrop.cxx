#include "sf4e__MenuBackdrop.hxx"

#include "../common/MenuBackdropRules.hxx"
#include "../common/MovieFrames.hxx"
#include "../platform/MovieFrameMailbox.hxx"
#include "../ui/GameMenu.hxx"
#include "../ui/SelectionArt.hxx"

#include <windows.h>

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace sf4e { namespace menu_backdrop {

namespace {

namespace frames = sf4e::movie_frames;
namespace mailbox = sf4e::platform::movie_frames;
using backdrop::Cycle;

double Seconds() {
    return static_cast<double>(GetTickCount64()) / 1000.0;
}

// How long the launcher may take over one frame before it counts as failed.
constexpr double kDecodeSeconds = 30;

// The one picture loading. Asking: the launcher decodes the frame into the
// cache folder. Reading: SelectionArt reads the still, or the frame's file.
struct Load {
    enum class Step { Asking, Reading };
    int entry = -1;
    Step step = Step::Reading;
    std::uint32_t request = 0;
    double since = 0;
    bool cached = false; // a frame file an earlier decode left
};

// Frame, the drawer and ArtGone run on the overlay's thread, and ArtGone
// also where the overlay is freed: one lock for all.
std::mutex s_mutex;
bool s_installed = false;
Cycle s_cycle;
backdrop::DrawWatch s_watch;
std::uint64_t s_presented = 0;
ui::SelectionArt* s_art = nullptr;
bool s_allowed = false, s_wanted = true;
std::string s_screen;
int s_advancedFrame = -1;
std::optional<Load> s_load;
frames::SourceHealth s_health;
bool s_givenUp = false;
mailbox::MovieFrameClient s_mailbox;
std::wstring s_cache;
// The scrim, easing from one kind of page's to another's.
backdrop::Scrim s_scrim, s_scrimFrom, s_scrimTo;
float s_scrimProgress = 1;
bool s_scrimSet = false;

std::string NameOf(int entry) {
    const int frame = frames::FrameOf(entry);
    if (frame < 0) return "the game's still";
    const auto& f = frames::Curated()[static_cast<std::size_t>(frame)];
    return std::string(f.movie) + " at " + std::to_string(f.ms) + " ms";
}

std::string KeyOf(int entry) {
    const int frame = frames::FrameOf(entry);
    if (frame < 0) return ui::SelectionArt::BackdropStillKey;
    return "backdrop/" + frames::FileName(frames::Curated()[static_cast<std::size_t>(frame)]);
}

std::wstring FileOf(int entry) {
    const int frame = frames::FrameOf(entry);
    if (frame < 0 || s_cache.empty()) return std::wstring();
    const std::string name = frames::FileName(frames::Curated()[static_cast<std::size_t>(frame)]);
    return s_cache + L"\\" + std::wstring(name.begin(), name.end());
}

// The picture of `entry` as the art has it now: its texture once read,
// missing once it never will be.
ui::SelectionImage PictureOf(int entry) {
    if (!s_art || entry < 0) return {};
    if (entry == frames::kStillEntry) return s_art->BackdropStill();
    return s_art->BackdropFrame(KeyOf(entry), FileOf(entry));
}

void Release(int entry) {
    if (s_art && entry >= 0) s_art->ReleaseBackdrop(KeyOf(entry));
}

// The cycle's pictures let go, and their textures with them.
void Settle() {
    for (const int entry : s_cycle.TakeReleases()) Release(entry);
}

void GiveUpFrames(const char* why) {
    if (s_givenUp) return;
    s_givenUp = true;
    frames::DropFrames(s_cycle);
    spdlog::info("Menu backdrop: movie frames given up for this session ({}); the game's still and Ember's art remain", why);
}

// The load under way ends: its picture kept or let go.
void Finish(bool ok, const std::string& why) {
    const Load load = *s_load;
    s_load.reset();
    const bool frame = frames::FrameOf(load.entry) >= 0;
    if (ok) {
        if (frame) s_health.Decoded();
        if (!s_cycle.Loaded(load.entry, true)) Release(load.entry);
        return;
    }
    s_cycle.Loaded(load.entry, false);
    Release(load.entry);
    spdlog::info("Menu backdrop: {} skipped: {}", NameOf(load.entry), why);
    if (!frame) return;
    // A cached frame that does not read goes, so a later session decodes it again.
    if (load.cached) DeleteFileW(FileOf(load.entry).c_str());
    s_health.Failed();
    if (s_health.GivenUp()) GiveUpFrames("frames failed one after another");
}

// Everything let go: a load under way and both pictures.
void LetGo(const char* why) {
    const bool held = s_load || s_cycle.Current() >= 0 || s_cycle.NextReady();
    if (s_load) {
        s_cycle.Abandon(s_load->entry);
        Release(s_load->entry);
        s_load.reset();
    }
    s_cycle.Reset();
    Settle();
    if (held) spdlog::info("Menu backdrop: pictures let go ({})", why);
}

void PumpLoad() {
    if (!s_load || !s_art) return;
    Load& load = *s_load;
    if (load.step == Load::Step::Asking) {
        switch (s_mailbox.Read(load.request)) {
        case mailbox::Answer::None:
            if (Seconds() - load.since > kDecodeSeconds) Finish(false, "the launcher did not answer in time");
            return;
        case mailbox::Answer::Ready:
            load.step = Load::Step::Reading;
            break;
        case mailbox::Answer::Unavailable:
            s_health.Unavailable();
            Finish(false, "the launcher cannot decode the movies here");
            GiveUpFrames("no decoder for the movies");
            return;
        case mailbox::Answer::Failed:
            Finish(false, "the launcher could not decode it");
            return;
        }
    }
    const ui::SelectionImage picture = PictureOf(load.entry);
    if (picture.texture) Finish(true, std::string());
    else if (picture.missing) Finish(false, "its picture did not read");
}

void StartLoad() {
    if (s_load || !s_art || !s_allowed || !s_wanted || !s_watch.AskedWithin(backdrop::kAskedSeconds, Seconds())) return;
    const std::optional<int> wanted = s_cycle.Wanted();
    if (!wanted) return;
    Load load;
    load.entry = *wanted;
    load.since = Seconds();
    s_cycle.Started(load.entry);
    const int frame = frames::FrameOf(load.entry);
    if (frame < 0) {
        s_load = load; // the still: SelectionArt reads it
        return;
    }
    if (s_cache.empty()) s_cache = mailbox::CacheFolder();
    const std::wstring file = FileOf(load.entry);
    if (!file.empty() && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) {
        load.cached = true;
        s_load = load;
        return;
    }
    s_load = load;
    // Not in the cache yet: Ember's launcher decodes it.
    if (!s_mailbox.Opened() && !s_mailbox.Open(mailbox::MailboxName(GetCurrentProcessId()))) {
        s_health.Unavailable();
        Finish(false, "no launcher to decode it");
        GiveUpFrames("the game was not started by this version of Ember's launcher");
        return;
    }
    const auto& f = frames::Curated()[static_cast<std::size_t>(frame)];
    s_load->request = s_mailbox.Ask(f.movie, f.ms);
    if (!s_load->request) {
        Finish(false, "the request could not be sent");
        return;
    }
    s_load->step = Load::Step::Asking;
}

void EaseScrim(float seconds) {
    const backdrop::Scrim target = backdrop::ScrimOf(backdrop::PageKindOf(s_screen));
    if (!s_scrimSet) {
        s_scrim = s_scrimFrom = s_scrimTo = target;
        s_scrimProgress = 1;
        s_scrimSet = true;
        return;
    }
    if (target != s_scrimTo) {
        s_scrimFrom = s_scrim;
        s_scrimTo = target;
        s_scrimProgress = 0;
    }
    s_scrimProgress = (std::min)(1.f, s_scrimProgress + seconds / static_cast<float>(backdrop::kScrimSeconds));
    s_scrim = backdrop::Mix(s_scrimFrom, s_scrimTo, s_scrimProgress);
}

// Ember's ink (GameMenu's art tint), at `alpha` from 0 to 1.
ImU32 Ink(float alpha) {
    return IM_COL32(20, 19, 18, static_cast<int>(std::lround((std::max)(0.f, (std::min)(1.f, alpha)) * 255)));
}

void Picture(ImDrawList* draw, const ui::SelectionImage& picture, ImVec2 min, ImVec2 max, float alpha) {
    const backdrop::UvRect uv = backdrop::CoverFit(picture.width, picture.height, max.x - min.x, max.y - min.y);
    const backdrop::Tint tint = backdrop::kArtTint;
    draw->AddImage(picture.texture, min, max, ImVec2(uv.u0, uv.v0), ImVec2(uv.u1, uv.v1),
        IM_COL32(tint.r, tint.g, tint.b, static_cast<int>(std::lround((std::max)(0.f, (std::min)(1.f, alpha)) * 255))));
}

// Inside a page's DrawMenuBackdrop: the pictures shown and the page's scrim
// over the whole screen; false (Ember's own art) while the menu does not
// allow it, the setting is off or nothing has loaded yet.
bool Draw(ImDrawList* draw, ImVec2 min, ImVec2 max) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_allowed || !s_wanted || !s_art || s_cycle.Exhausted()) return false;
    // The clock runs once a frame, while the backdrop is on screen.
    if (ImGui::GetFrameCount() != s_advancedFrame) {
        s_advancedFrame = ImGui::GetFrameCount();
        s_watch.Asked();
        const float seconds = ImGui::GetIO().DeltaTime;
        s_cycle.Advance(seconds);
        Settle(); // what the step let go is not drawn this frame
        EaseScrim(seconds);
    }
    const Cycle::Frame shown = s_cycle.Shown();
    if (shown.Empty()) return false;
    const ui::SelectionImage under = PictureOf(shown.under.entry), over = PictureOf(shown.over.entry);
    // The scrim covers what the pictures cover: all of it over a picture, and
    // as much as the first picture has faded in over Ember's own art.
    float cover = 1;
    if (under.texture) {
        draw->AddRectFilled(min, max, Ink(1));
        Picture(draw, under, min, max, 1);
    } else {
        if (!ui::DrawMenuArt(draw, min, max)) draw->AddRectFilled(min, max, Ink(1));
        cover = over.texture ? shown.over.alpha : 0;
    }
    if (over.texture) Picture(draw, over, min, max, shown.over.alpha);
    draw->AddRectFilledMultiColor(min, max, Ink(s_scrim.topLeft * cover), Ink(s_scrim.topRight * cover),
        Ink(s_scrim.bottomRight * cover), Ink(s_scrim.bottomLeft * cover));
    s_watch.Drawn();
    return true;
}

} // namespace

void Install() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_installed) return;
    s_installed = true;
    // A new order each session.
    const auto seed = static_cast<std::uint32_t>(GetTickCount64() ^ (GetCurrentProcessId() * 2654435761u));
    s_cycle = Cycle(frames::EntryCount(), seed);
    std::string why;
    if (!frames::ListValid(frames::Curated(), why)) {
        spdlog::error("Menu backdrop: the movie frame list is not usable ({}); the still alone", why);
        GiveUpFrames("the list");
    }
    ui::SetMenuBackdropDrawer(Draw);
    spdlog::info("Menu backdrop: the game's still and {} movie frames", frames::Curated().size());
}

void Frame(const backdrop::Surface& surface, bool wanted, const std::string& screen, ui::SelectionArt* art) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_installed) return;
    s_watch.Presented(++s_presented, Seconds());
    s_art = art;
    s_allowed = art && backdrop::Allowed(surface);
    s_screen = screen;
    // Turned off: everything goes at once, and Ember's own art shows.
    if (!wanted && s_wanted) LetGo("Background artwork turned off");
    s_wanted = wanted;
    // Not drawn for a while (the menu closed, a fight): both pictures go, and
    // the next fades in from Ember's own art when the backdrop is back.
    if ((s_load || s_cycle.Current() >= 0 || s_cycle.NextReady()) && s_watch.IdleFor(backdrop::kIdleReleaseSeconds, Seconds()))
        LetGo("not shown for 5 s");
    Settle();
    PumpLoad();
    StartLoad();
}

void ArtGone() {
    std::lock_guard<std::mutex> lock(s_mutex);
    // The art's textures went with it: nothing to release into the new art.
    if (s_load) s_cycle.Abandon(s_load->entry);
    s_load.reset();
    s_cycle.Reset();
    s_cycle.TakeReleases();
    s_art = nullptr;
    s_allowed = false;
}

} }
