#pragma once
// Notifications: the one list of what Ember told the player and of what waits on
// them. Every source tells into it, and everything that shows it reads it: the
// cards over Ember's menus (ApplicationShell__Toasts.cxx), the Notifications tab,
// and Home's News row with its count and its words.
//
// - A one-off (Say) is a notice, or a toast whose buttons are its actions: it
//   waits while it has actions nobody took.
// - A source publishes everything it has waiting, by key and in its own order
//   (Publish): a request or an invite from friends. The source's own row lists
//   it while it waits; a key new to the list becomes a card, unless the source
//   says the player is already looking at the list or this is its first read;
//   a key the source no longer publishes is gone, and its words stay as said,
//   or go with it if they never showed.
// - Answering an item (its card's buttons or keys, its row) takes it once: its
//   card closes and its actions are not offered again.
// - An item that asks to be seen (`again`) is shown once more on the next free
//   screen when Ember hid its card before its time (Conceal), or when its card
//   with buttons showed over the post-match prompt, whose keys are not its own.
//
// No ImGui here: the clock is the interface's, in seconds, passed in, so the
// rules are tested without a frame (notifications_test.cxx).
#include "ShellAction.hxx"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace ui {
enum class Tone;

// What a toast is about, said over it (Toasts board); None says nothing.
enum class ToastKind { None, Invitation, Room, Update, Match, System };
// A toast's button: it opens `screen` at its `row`, where the player acts on the
// notice, and sends `commands` as they are, in order. One with neither is Later,
// which only puts the toast away. A source's item answers through its source.
struct ToastAction { std::string label, screen, row; std::vector<RuntimeRequest> commands; };
// How long a card stays up: a notice briefly, one with buttons long enough to reach them.
constexpr double ToastSeconds = 4;
constexpr double ActionToastSeconds = 8;

class Notifications {
public:
    enum class State { Waiting, Answered, Gone };
    // The words said are kept for the tab up to this many; what waits is kept while it waits.
    static constexpr std::size_t Kept = 20;

    struct Item {
        // The source's key ("request:rq_1"), which is also its row's id; a one-off's is "toast-<id>".
        std::string key;
        std::string text;
        Tone tone{};
        ToastKind kind = ToastKind::None;
        std::vector<ToastAction> actions;
        // Shown once more on the next free screen when it was not seen (see above).
        bool again = false;

        // The list's own: the item's number (its card's), its source (empty for a one-off), when it was said and
        // when its card ends, its state, its place in its source's order, and whether it is owed a showing, has had
        // its second, or ever showed at all.
        unsigned id = 0;
        std::string source;
        double at = 0, until = 0;
        State state = State::Waiting;
        std::size_t rank = 0;
        bool unseen = false, retold = false, shown = false;

        // The source lists it, so its row there stands for it.
        bool Sourced() const { return !source.empty() && state != State::Gone; }
        // It waits on the player: listed by its source, or a one-off whose actions nobody took.
        bool Waits() const { return Sourced() || (source.empty() && state == State::Waiting && !actions.empty()); }
        // Its buttons can still be pressed.
        bool Answerable() const { return state == State::Waiting && !actions.empty(); }
        // Its card is up at `now`.
        bool Card(double now) const { return now < until; }
        // The first line, which says it; the rest sits under it.
        std::string Head() const { return text.substr(0, text.find('\n')); }
    };

    // A one-off, said at `now`: a card at once. Returns its id.
    unsigned Say(Item item, double now) {
        item.id = ++count_;
        item.key = "toast-" + std::to_string(item.id);
        item.source.clear();
        item.state = State::Waiting;
        Card(item, now);
        items_.push_back(std::move(item));
        Trim();
        return items_.back().id;
    }

    // Everything `source` has waiting now, in its order. `quiet`: the player is looking at a screen that lists them,
    // or this is the source's first read, so nothing new becomes a card and nothing is owed a showing.
    void Publish(const std::string& source, std::vector<Item> items, double now, bool quiet) {
        // The keys it publishes, taken before the items are moved into the list.
        std::vector<std::string> published;
        for (const auto& next : items) published.push_back(next.key);
        for (std::size_t rank = 0; rank < items.size(); ++rank) {
            auto& next = items[rank];
            auto* known = Keyed(next.key);
            if (known && known->source == source) {
                known->text = std::move(next.text);
                known->tone = next.tone;
                known->kind = next.kind;
                known->actions = std::move(next.actions);
                known->again = next.again;
                known->rank = rank;
                // Back after it was gone (the list was read again): it waits again, and is not new.
                if (known->state == State::Gone) known->state = State::Waiting;
                if (quiet) Seen(*known);
                continue;
            }
            next.id = ++count_;
            next.source = source;
            next.rank = rank;
            next.state = State::Waiting;
            next.at = next.until = now;
            if (!quiet) Card(next, now);
            items_.push_back(std::move(next));
        }
        for (auto& item : items_) {
            if (item.source != source || item.state == State::Gone) continue;
            if (std::find(published.begin(), published.end(), item.key) == published.end()) {
                item.state = State::Gone;
                item.until = (std::min)(item.until, now);
                item.unseen = false;
            }
        }
        // Gone without ever showing: nothing was said of it.
        items_.erase(std::remove_if(items_.begin(), items_.end(), [](const Item& item) { return item.state == State::Gone && !item.shown; }), items_.end());
        Trim();
    }

    // The player took one of item `id`'s actions (Later is one): its card closes and it is answered. The item, whose
    // actions the caller carries out, or null when it cannot be answered (no such item, no actions, answered or gone).
    const Item* Answer(unsigned id, double now) {
        auto* item = At(id);
        if (!item || !item->Answerable()) return nullptr;
        item->state = State::Answered;
        item->until = (std::min)(item->until, now);
        item->unseen = false;
        return item;
    }

    // Ember is hidden at `now`: a card that asks to be seen and is still up is owed a showing.
    void Conceal(double now) {
        for (auto& item : items_)
            if (Owed(item) && item.Card(now)) item.unseen = true;
    }

    // Ember shows at `now`: over the post-match prompt (`prompt`) a card with buttons that asks to be seen cannot be
    // answered by key, so it is owed a showing; on a free screen (`free`) whatever is owed is shown again, once.
    void Show(double now, bool prompt, bool free) {
        for (auto& item : items_) {
            if (prompt && Owed(item) && !item.actions.empty() && item.Card(now)) item.unseen = true;
            if (free && item.unseen && item.state == State::Waiting) {
                item.unseen = false;
                item.retold = true;
                Card(item, now);
            }
        }
    }

    // The cards that came up since the last call, oldest first, to be read out and (the newest with buttons) focused.
    std::vector<unsigned> TakeAppeared() { return std::exchange(appeared_, {}); }

    // What waits on the player, and the first of it in the tab's order.
    int Waiting() const { return static_cast<int>(std::count_if(items_.begin(), items_.end(), [](const Item& item) { return item.Waits(); })); }
    const Item* FirstWaiting() const {
        const auto listed = Listed();
        const auto first = std::find_if(listed.begin(), listed.end(), [](const Item* item) { return item->Waits(); });
        return first == listed.end() ? nullptr : *first;
    }
    // The newest item said, or null.
    const Item* Newest() const {
        const auto said = Said();
        return said.empty() ? nullptr : said.front();
    }
    // The tab's order: what the sources list, each in its order, then everything said, newest first.
    std::vector<const Item*> Listed() const {
        std::vector<const Item*> sourced, said = Said();
        for (const auto& item : items_) if (item.Sourced()) sourced.push_back(&item);
        std::stable_sort(sourced.begin(), sourced.end(), [](const Item* a, const Item* b) { return a->source != b->source ? a->source < b->source : a->rank < b->rank; });
        sourced.insert(sourced.end(), said.begin(), said.end());
        return sourced;
    }
    // The cards up at `now`, newest first.
    std::vector<const Item*> Cards(double now) const {
        std::vector<const Item*> cards;
        for (const auto& item : items_) if (item.Card(now)) cards.push_back(&item);
        std::stable_sort(cards.begin(), cards.end(), Newer);
        return cards;
    }
    const Item* Find(unsigned id) const {
        const auto found = std::find_if(items_.begin(), items_.end(), [&](const Item& item) { return item.id == id; });
        return found == items_.end() ? nullptr : &*found;
    }

    // The interface's clock was set back (a device reset): every time it holds is moved with it.
    template <class Moved> void Rebase(Moved moved) {
        for (auto& item : items_) { item.at = moved(item.at); item.until = moved(item.until); }
    }

private:
    static bool Newer(const Item* a, const Item* b) { return a->at != b->at ? a->at > b->at : a->id > b->id; }
    static bool Owed(const Item& item) { return item.again && !item.retold && item.state == State::Waiting; }
    static void Seen(Item& item) { item.unseen = false; item.retold = true; }

    void Card(Item& item, double now) {
        item.at = now;
        item.until = now + (item.actions.empty() ? ToastSeconds : ActionToastSeconds);
        item.shown = true;
        appeared_.push_back(item.id);
    }
    Item* At(unsigned id) {
        const auto found = std::find_if(items_.begin(), items_.end(), [&](const Item& item) { return item.id == id; });
        return found == items_.end() ? nullptr : &*found;
    }
    Item* Keyed(const std::string& key) {
        const auto found = std::find_if(items_.begin(), items_.end(), [&](const Item& item) { return item.key == key; });
        return found == items_.end() ? nullptr : &*found;
    }
    // Everything said that its source does not list, newest first.
    std::vector<const Item*> Said() const {
        std::vector<const Item*> said;
        for (const auto& item : items_) if (!item.Sourced()) said.push_back(&item);
        std::stable_sort(said.begin(), said.end(), Newer);
        return said;
    }
    // Past Kept items said, the oldest go; what a source lists stays.
    void Trim() {
        while (std::count_if(items_.begin(), items_.end(), [](const Item& item) { return !item.Sourced(); }) > static_cast<std::ptrdiff_t>(Kept)) {
            const auto oldest = std::min_element(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
                if (a.Sourced() != b.Sourced()) return !a.Sourced();
                return a.at != b.at ? a.at < b.at : a.id < b.id;
            });
            items_.erase(oldest);
        }
    }

    std::vector<Item> items_;
    std::vector<unsigned> appeared_;
    unsigned count_ = 0;
};
} }
