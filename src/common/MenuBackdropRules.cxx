#include "MenuBackdropRules.hxx"

#include <algorithm>
#include <initializer_list>

namespace sf4e { namespace backdrop {

namespace {

float Clamp01(float value) {
    return value < 0 ? 0 : value > 1 ? 1 : value;
}

} // namespace

bool Allowed(const Surface& surface) {
    return surface.shellShown && surface.mainMenu && !surface.fighting && !surface.watching && !surface.replaying;
}

PageKind PageKindOf(std::string_view screen) {
    // Home's rows, Online play's and Training select's: a column on the left,
    // the screen open beside it.
    for (std::string_view column : {"home", "online", "training"}) {
        if (screen == column) return PageKind::Column;
    }
    return PageKind::Dense;
}

Scrim ScrimOf(PageKind kind) {
    // A column: heaviest behind the rows on the left and the hint bar,
    // lightest at the top right, where the screen is open. A dense page:
    // even, a little heavier at the bottom. SFV Ember's values.
    if (kind == PageKind::Column) return {0.93f, 0.60f, 0.77f, 0.96f};
    return {0.84f, 0.84f, 0.89f, 0.89f};
}

Scrim Mix(const Scrim& from, const Scrim& to, float t) {
    t = Clamp01(t);
    const auto mix = [t](float a, float b) { return a + (b - a) * t; };
    return {mix(from.topLeft, to.topLeft), mix(from.topRight, to.topRight), mix(from.bottomRight, to.bottomRight),
        mix(from.bottomLeft, to.bottomLeft)};
}

float ScrimAt(const Scrim& scrim, float x, float y) {
    x = Clamp01(x);
    y = Clamp01(y);
    const float top = scrim.topLeft + (scrim.topRight - scrim.topLeft) * x;
    const float bottom = scrim.bottomLeft + (scrim.bottomRight - scrim.bottomLeft) * x;
    return top + (bottom - top) * y;
}

UvRect CoverFit(int width, int height, float targetWidth, float targetHeight, float anchorX, float anchorY) {
    UvRect uv;
    if (width <= 0 || height <= 0 || !(targetWidth > 0) || !(targetHeight > 0)) return uv;
    const float picture = (float)width / (float)height, target = targetWidth / targetHeight;
    if (target > picture) {
        // A wider screen: the picture's whole width, a band of its height.
        const float span = picture / target;
        uv.v0 = (1 - span) * Clamp01(anchorY);
        uv.v1 = uv.v0 + span;
    } else {
        // A narrower one (or the same shape): its whole height, a band of its width.
        const float span = target / picture;
        uv.u0 = (1 - span) * Clamp01(anchorX);
        uv.u1 = uv.u0 + span;
    }
    return uv;
}

float FadeCurve(float progress) {
    const float t = Clamp01(progress);
    return t * t * (3 - 2 * t);
}

Cycle::Cycle(int entries, std::uint32_t seed, Timing timing)
    : entries_(entries > 0 ? entries : 0), timing_(timing), random_(seed), failed_((size_t)entries_, false) {
    Refill();
}

int Cycle::Pick() {
    // Through the shuffled order, a new shuffle each time it runs out; never
    // a failed picture, never the one shown, and never the one picked last
    // first in a new shuffle.
    for (int pass = 0; pass < 2; pass++) {
        while (cursor_ < order_.size()) {
            const int entry = order_[cursor_++];
            if (failed_[(size_t)entry] || entry == current_.entry) continue;
            lastPicked_ = entry;
            return entry;
        }
        order_.resize((size_t)entries_);
        for (int i = 0; i < entries_; i++) order_[(size_t)i] = i;
        std::shuffle(order_.begin(), order_.end(), random_);
        if (order_.size() > 1 && order_.front() == lastPicked_) {
            std::uniform_int_distribution<size_t> other(1, order_.size() - 1);
            std::swap(order_.front(), order_[other(random_)]);
        }
        cursor_ = 0;
    }
    return -1;
}

void Cycle::Refill() {
    if (next_.entry >= 0 || fading_ || Exhausted()) return;
    next_ = {Pick(), false, false};
}

std::optional<int> Cycle::Wanted() const {
    if (next_.entry < 0 || next_.loading || next_.ready || fading_ || stale_ >= 0) return std::nullopt;
    if (clock_ < settleUntil_ || clock_ < retryAt_) return std::nullopt;
    return next_.entry;
}

void Cycle::Started(int entry) {
    if (entry == next_.entry && !next_.loading && !next_.ready) next_.loading = true;
}

void Cycle::Skip(int entry) {
    if (failed_[(size_t)entry]) return;
    failed_[(size_t)entry] = true;
    failedCount_++;
}

bool Cycle::Loaded(int entry, bool ok) {
    if (entry >= 0 && entry == stale_) {
        stale_ = -1;
        return false;
    }
    if (entry < 0 || entry != next_.entry || !next_.loading) return false;
    next_.loading = false;
    if (ok) {
        next_.ready = true;
        return true;
    }
    Skip(entry);
    next_ = {};
    retryAt_ = clock_ + timing_.retry;
    Refill();
    return false;
}

void Cycle::Abandon(int entry) {
    if (entry < 0) return;
    if (entry == stale_) stale_ = -1;
    else if (entry == next_.entry && next_.loading) next_.loading = false;
}

void Cycle::Drop(int entry) {
    if (entry < 0 || entry >= entries_) return;
    Skip(entry);
    // The next picture chosen and not yet loading or ready is chosen again.
    if (entry == next_.entry && !next_.loading && !next_.ready && !fading_) {
        next_ = {};
        Refill();
    }
}

void Cycle::StartFade() {
    fading_ = true;
    faded_ = 0;
}

void Cycle::Advance(double seconds) {
    const double step = seconds < 0 ? 0 : seconds > kMostStep ? kMostStep : seconds;
    clock_ += step;
    if (fading_) {
        faded_ += step;
        if (faded_ < timing_.fade) return;
        if (current_.ready) releases_.push_back(current_.entry);
        current_ = next_;
        next_ = {};
        fading_ = stepWanted_ = false;
        faded_ = held_ = 0;
        settleUntil_ = clock_ + timing_.settle;
        Refill();
        return;
    }
    if (current_.ready) held_ += step;
    if (next_.ready && (!current_.ready || held_ >= timing_.hold || stepWanted_)) StartFade();
    Refill();
}

void Cycle::Step() {
    stepWanted_ = true;
    settleUntil_ = clock_;
    retryAt_ = clock_;
}

void Cycle::Reset() {
    if (current_.ready) releases_.push_back(current_.entry);
    if (next_.ready) releases_.push_back(next_.entry);
    if (next_.loading) stale_ = next_.entry;
    current_ = next_ = {};
    fading_ = stepWanted_ = false;
    faded_ = held_ = 0;
    Refill();
}

Cycle::Frame Cycle::Shown() const {
    Frame frame;
    if (current_.ready) frame.under = {current_.entry, 1.f};
    if (fading_ && next_.ready) frame.over = {next_.entry, FadeCurve((float)(faded_ / timing_.fade))};
    return frame;
}

std::vector<int> Cycle::TakeReleases() {
    std::vector<int> taken;
    taken.swap(releases_);
    return taken;
}

int Cycle::Alive() const {
    return (current_.ready ? 1 : 0) + (next_.loading || next_.ready ? 1 : 0) + (stale_ >= 0 ? 1 : 0);
}

bool Cycle::Exhausted() const {
    return entries_ == 0 || failedCount_ >= entries_;
}

void DrawWatch::Presented(std::uint64_t frame, double now) {
    frame_ = frame;
    now_ = now;
}

void DrawWatch::Asked() {
    asked_ = true;
    askedAt_ = now_;
}

void DrawWatch::Drawn() {
    drawn_ = true;
    drawnFrame_ = frame_;
}

bool DrawWatch::DrawnLastFrame() const {
    return drawn_ && frame_ - drawnFrame_ <= 1;
}

bool DrawWatch::AskedWithin(double seconds, double now) const {
    return asked_ && now - askedAt_ <= seconds;
}

bool DrawWatch::IdleFor(double seconds, double now) const {
    return !asked_ || now - askedAt_ >= seconds;
}

} }
