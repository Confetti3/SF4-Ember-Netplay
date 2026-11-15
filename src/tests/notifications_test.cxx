// Notifications (ui/Notifications.hxx), without a frame: what is listed and in which order, what waits and so is
// counted, which cards are up, what answering does, and the rule that shows a card once more when it was not seen.
#include "../ui/Notifications.hxx"

#include <cstdio>
#include <string>
#include <vector>

#include "test_support.hxx"

using sf4e::ui::Notifications;
using sf4e::ui::ToastAction;
using sf4e::ui::ToastSeconds;
using sf4e::ui::ActionToastSeconds;
using Item = Notifications::Item;
using State = Notifications::State;

namespace {
Item Words(const std::string& text, bool again = false) {
    Item item;
    item.text = text;
    item.again = again;
    return item;
}
Item Asking(const std::string& key, const std::string& text) {
    Item item;
    item.key = key;
    item.text = text;
    item.actions = {ToastAction{"Accept"}, ToastAction{"Later"}};
    item.again = true;
    return item;
}
std::vector<std::string> Keys(const Notifications& n) {
    std::vector<std::string> keys;
    for (const auto* item : n.Listed()) keys.push_back(item->key);
    return keys;
}
}

// A one-off is a card at once, read out once; a notice waits for nothing, a toast with actions until answered.
void OneOffs() {
    Notifications n;
    const auto notice = n.Say(Words("Link copied"), 10);
    Item asking = Words("Room invitation");
    asking.actions = {ToastAction{"Join", "home", ""}, ToastAction{"Later"}};
    const auto invitation = n.Say(asking, 11);
    EXPECT(n.Find(notice)->key == "toast-" + std::to_string(notice) && n.Find(notice)->Card(10 + ToastSeconds - 0.01) && !n.Find(notice)->Card(10 + ToastSeconds));
    EXPECT(n.Find(invitation)->Card(11 + ActionToastSeconds - 0.01) && !n.Find(invitation)->Card(11 + ActionToastSeconds));
    EXPECT((n.TakeAppeared() == std::vector<unsigned>{notice, invitation}) && n.TakeAppeared().empty());
    EXPECT(n.Waiting() == 1 && n.FirstWaiting()->id == invitation && n.Newest()->id == invitation);
    EXPECT((Keys(n) == std::vector<std::string>{"toast-" + std::to_string(invitation), "toast-" + std::to_string(notice)}));
    // Answered: once, its card goes, nothing waits; a notice cannot be answered.
    const auto* answered = n.Answer(invitation, 12);
    EXPECT(answered && answered->state == State::Answered && !answered->Card(12) && answered->actions.size() == 2);
    EXPECT(!n.Answer(invitation, 12) && !n.Answer(notice, 12) && !n.Answer(9999, 12));
    EXPECT(n.Waiting() == 0 && !n.FirstWaiting() && n.Find(invitation)->Head() == "Room invitation");
    // Only the newest Kept said are kept.
    for (int i = 0; i < 30; ++i) n.Say(Words("Notice " + std::to_string(i)), 20 + i);
    EXPECT(n.Listed().size() == Notifications::Kept && n.Newest()->text == "Notice 29" && !n.Find(notice));
}

// A source's items: new ones are cards unless quiet; they wait while published, in the source's order, before the
// words said; one no longer published is gone, keeping its words if it showed and leaving nothing if it never did.
void Sourced() {
    Notifications n;
    n.Say(Words("Link copied"), 1);
    n.Publish("friends", {Asking("request:rq_1", "Player 9 wants to be friends\nPlayed you 6 games")}, 2, true);
    EXPECT(n.Waiting() == 1 && n.Cards(2).size() == 1 && n.Cards(2)[0]->text == "Link copied");
    n.TakeAppeared();
    n.Publish("friends", {Asking("request:rq_2", "Player 2 wants to be friends"), Asking("request:rq_1", "Player 9 wants to be friends\nPlayed you 7 games")}, 3, false);
    const auto appeared = n.TakeAppeared();
    EXPECT(appeared.size() == 1 && n.Find(appeared[0])->key == "request:rq_2" && n.Find(appeared[0])->Card(3));
    EXPECT(n.Waiting() == 2 && n.FirstWaiting()->key == "request:rq_2" && n.FirstWaiting()->Head() == "Player 2 wants to be friends");
    // The source's order, then what was said; its words follow what it publishes.
    const auto keys = Keys(n);
    EXPECT(keys.size() == 3 && keys[0] == "request:rq_2" && keys[1] == "request:rq_1" && keys[2].compare(0, 6, "toast-") == 0);
    EXPECT(n.Listed()[1]->text == "Player 9 wants to be friends\nPlayed you 7 games" && n.Listed()[0]->Sourced());
    // Later on rq_2's card: answered, its card goes, but it still waits in the list.
    const auto rq2 = n.Listed()[0]->id;
    EXPECT(n.Answer(rq2, 4) && n.Find(rq2)->Sourced() && n.Find(rq2)->Waits() && n.Waiting() == 2 && !n.Find(rq2)->Card(4));
    // Accepted elsewhere: gone from the source. The one that showed keeps its words, without actions to take; the
    // one first read quietly leaves nothing.
    n.Publish("friends", {}, 5, false);
    EXPECT(n.Waiting() == 0 && n.Find(rq2) && n.Find(rq2)->state == State::Gone && !n.Find(rq2)->Answerable() && !n.Find(rq2)->Sourced());
    EXPECT((Keys(n).size() == 2) && n.Newest()->key == "request:rq_2");
    // Back again (the list read once more): it waits, and is no news.
    n.Publish("friends", {Asking("request:rq_2", "Player 2 wants to be friends")}, 6, false);
    EXPECT(n.TakeAppeared().empty() && n.Waiting() == 1 && n.Find(rq2)->state == State::Waiting && !n.Find(rq2)->Card(6));
}

// Shown once more: a card that asks to be seen, hidden before its time or with buttons over the post-match prompt, is
// shown again on the next free screen, once; one seen to its end, answered, gone or listed is not.
void Again() {
    {
        Notifications n;
        const auto words = n.Say(Words("Request sent to Player 2.", true), 10);
        const auto plain = n.Say(Words("Link copied"), 10);
        n.TakeAppeared();
        n.Conceal(11);
        n.Show(70, false, false);
        EXPECT(n.Cards(70).empty() && n.TakeAppeared().empty());
        n.Show(80, false, true);
        EXPECT(n.Cards(80).size() == 1 && n.Cards(80)[0]->id == words && (n.TakeAppeared() == std::vector<unsigned>{words}));
        EXPECT(!n.Find(plain)->Card(80) && n.Find(words)->at == 80);
        // Once only.
        n.Conceal(81);
        n.Show(90, false, true);
        EXPECT(n.TakeAppeared().empty());
    }
    {
        // Seen to its end before Ember hid: not owed.
        Notifications n;
        n.Say(Words("Request sent to Player 2.", true), 10);
        n.Conceal(10 + ToastSeconds);
        n.Show(80, false, true);
        n.TakeAppeared();
        EXPECT(n.Cards(80).empty());
    }
    {
        // Over the prompt: a card with buttons is owed; words are read there.
        Notifications n;
        const auto words = n.Say(Words("Request sent to Player 2.", true), 10);
        n.Publish("friends", {Asking("request:rq_1", "Player 9 wants to be friends")}, 10, false);
        n.TakeAppeared();
        n.Show(11, true, false);
        n.Show(30, false, true);
        const auto shown = n.TakeAppeared();
        EXPECT(shown.size() == 1 && n.Find(shown[0])->key == "request:rq_1" && !n.Find(words)->Card(30));
    }
    {
        // Answered, gone, or shown on a screen that lists it: not owed.
        Notifications n;
        n.Publish("friends", {Asking("request:rq_1", "Player 9 wants to be friends"), Asking("request:rq_2", "Player 2"), Asking("request:rq_3", "Player 3")}, 10, false);
        const auto ids = n.TakeAppeared();
        n.Conceal(11);
        n.Answer(ids[0], 12);
        n.Publish("friends", {Asking("request:rq_1", "Player 9 wants to be friends"), Asking("request:rq_3", "Player 3")}, 13, false);
        n.Publish("friends", {Asking("request:rq_1", "Player 9 wants to be friends"), Asking("request:rq_3", "Player 3")}, 14, true);
        n.Show(20, false, true);
        EXPECT(n.TakeAppeared().empty());
    }
}

// A clock set back moves every time the list holds.
void Rebase() {
    Notifications n;
    const auto id = n.Say(Words("Link copied"), 100);
    n.Rebase([](double t) { return t - 90; });
    EXPECT(n.Find(id)->at == 10 && n.Find(id)->Card(10) && !n.Find(id)->Card(10 + ToastSeconds));
}

int main() {
    OneOffs();
    Sourced();
    Again();
    Rebase();
    if (TEST_FAILURES()) std::printf("%d failure(s)\n", TEST_FAILURES());
    else std::printf("notifications tests passed\n");
    return TEST_EXIT_CODE();
}
