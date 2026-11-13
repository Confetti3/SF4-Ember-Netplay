#include "FriendsPanel.hxx"
#include "MenuRows.hxx"
#include "PlayerCard.hxx"
#include "ShellView.hxx"
#include "Theme.hxx"
#include "../common/LocalTime.hxx"
#include "../common/Localization.hxx"
#include "../netplay/FriendVisits.hxx"
#include <algorithm>
#include <cctype>
#include <type_traits>
#include <utility>

namespace sf4e { namespace ui {
namespace friends_panel {
const nf::State& StateOf(const nf::View& view) {
    static const nf::State none;
    return view.state.value ? *view.state.value : none;
}

namespace {
std::string Lower(const std::string& text) {
    std::string out = text;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
bool NameBefore(const nf::Friend* a, const nf::Friend* b) {
    const std::string x = Lower(a->name), y = Lower(b->name);
    return x != y ? x < y : a->id < b->id;
}

// Why a choice that goes to a friend's room cannot be taken, as the words the runtime's own refusals use.
const char* WhyNotThere(const nf::Destination& destination, bool watch) {
    using Refusal = nf::Destination::Refusal;
    switch (destination.refusal) {
    case Refusal::PrivateRoom: return "friends.private_invite_only";
    case Refusal::None: return nullptr;
    default: return watch ? "friends.cannot_watch" : "friends.cannot_join";
    }
}
MenuChoice Choice(const char* id, const char* label, bool enabled, const std::string& detail) { return {id, loc::T(label), detail, enabled}; }
}

Mode ModeOf(const nf::View& view, bool service) {
    const auto& state = StateOf(view);
    if (state.kind == nf::State::Kind::NoAccount) return Mode::NoAccount;
    if (state.kind == nf::State::Kind::Off) return Mode::Off;
    if (state.kind == nf::State::Kind::Listed) return Mode::Listed;
    // Unavailable: the service said it has no friends, there is no service to ask, or nothing has been answered yet.
    if (view.state.loaded || (!service && view.bridge.empty())) return Mode::Unavailable;
    return !view.state.error.empty() && !view.state.loading ? Mode::Unreachable : Mode::Loading;
}

bool Stale(const nf::View& view) {
    // An action the bridge refused leaves its code in `error` too, and the state it kept is current.
    return StateOf(view).kind == nf::State::Kind::Listed && !view.state.error.empty() && view.state.error != view.refusal;
}

Gate GateOf(const nf::View& view, bool service, bool pending) {
    Gate gate;
    const auto mode = ModeOf(view, service);
    gate.go = mode == Mode::Listed && !Stale(view);
    gate.act = gate.go && !view.busy && !pending;
    // Friends turned off are not "unreachable": the words say so, and how to go on.
    const char* unusable = mode == Mode::Off ? "friends.off" : "friends.unreachable";
    gate.goWhy = gate.go ? nullptr : unusable;
    gate.actWhy = gate.act ? nullptr : !gate.go ? unusable : "friends.busy";
    return gate;
}

const char* ModeWords(Mode mode) {
    switch (mode) {
    case Mode::NoAccount: return "friends.no_account";
    case Mode::Off: return "friends.off";
    case Mode::Unavailable: return "friends.unavailable";
    case Mode::Loading: return "friends.loading";
    case Mode::Unreachable: return "friends.unreachable";
    case Mode::Listed: break;
    }
    return nullptr;
}

Gate MemberGateOf(const nf::View& view, bool service, bool pending) {
    Gate gate;
    const auto mode = ModeOf(view, service);
    gate.go = mode == Mode::Listed;
    gate.act = gate.go && !view.busy && !pending;
    gate.goWhy = ModeWords(mode);
    gate.actWhy = gate.act ? nullptr : !gate.go ? gate.goWhy : "friends.busy";
    return gate;
}

Groups Arrange(const nf::State& state) {
    Groups groups;
    for (const auto& request : state.incoming) groups.requests.push_back(&request);
    std::stable_sort(groups.requests.begin(), groups.requests.end(), [](const nf::Incoming* a, const nf::Incoming* b) { return a->sentAt != b->sentAt ? a->sentAt > b->sentAt : a->id < b->id; });
    for (const auto& entry : state.friends) {
        switch (entry.presence.kind) {
        case nf::Presence::Kind::InRoom: groups.inRoom.push_back(&entry); break;
        case nf::Presence::Kind::Online: groups.online.push_back(&entry); break;
        case nf::Presence::Kind::Offline: groups.offline.push_back(&entry); break;
        }
    }
    std::sort(groups.inRoom.begin(), groups.inRoom.end(), [](const nf::Friend* a, const nf::Friend* b) {
        const bool x = Joinable(*a), y = Joinable(*b);
        return x != y ? x : NameBefore(a, b);
    });
    std::sort(groups.online.begin(), groups.online.end(), NameBefore);
    std::sort(groups.offline.begin(), groups.offline.end(), [](const nf::Friend* a, const nf::Friend* b) {
        return a->presence.seenAt != b->presence.seenAt ? a->presence.seenAt > b->presence.seenAt : NameBefore(a, b);
    });
    return groups;
}

OtherCardView CardOf(const nf::Friend& entry, FighterArt art) {
    OtherCardView card;
    card.name = entry.name;
    card.country = entry.country;
    card.badge = entry.badge;
    card.fighter = entry.hasMain ? entry.main.fighter : -1;
    card.art = art;
    const bool main = entry.hasMain && entry.main.showsPoints;
    card.points = main || entry.hasAccount;
    card.fighterPoints = main ? entry.main.points : 0;
    card.fighterTier = main ? entry.main.tier : 0;
    card.accountKnown = entry.hasAccount;
    card.accountPoints = entry.account.points;
    return card;
}

bool Joinable(const nf::Friend& entry) {
    const auto& presence = entry.presence;
    return presence.kind == nf::Presence::Kind::InRoom && presence.hasRoom && !presence.privateRoom && presence.room.joinable;
}

Press PressOf(const nf::Friend& entry, bool canInvite) {
    if (entry.presence.kind == nf::Presence::Kind::Offline) return Press::More;
    if (Joinable(entry)) return Press::Join;
    return canInvite ? Press::Invite : Press::More;
}

std::string IdOf(const std::string& row, const char* prefix) { return HasPrefix(row, prefix) ? row.substr(std::string(prefix).size()) : std::string(); }

std::string Ago(std::uint64_t now, std::uint64_t at) {
    if (!now || !at) return {};
    const std::uint64_t seconds = now > at ? now - at : 0;
    return seconds < 24 * 3600 ? loc::Tf("friends.hours_ago", (std::max<std::uint64_t>)(1, seconds / 3600)) : loc::Tf("friends.days_ago", seconds / (24 * 3600));
}

std::string Status(const nf::Friend& entry, std::uint64_t now) {
    const auto& presence = entry.presence;
    switch (presence.kind) {
    case nf::Presence::Kind::Offline: {
        const std::string ago = Ago(now, presence.seenAt);
        return ago.empty() ? std::string() : loc::Tf("friends.last_online", ago);
    }
    case nf::Presence::Kind::Online:
        return loc::T(presence.activity == nf::Activity::OfflinePlay ? "friends.offline_play" : "friends.menus");
    case nf::Presence::Kind::InRoom:
        break;
    }
    // A room that is neither public and listed nor private is no room to name.
    if (presence.privateRoom || !presence.hasRoom) return loc::T("friends.private_room");
    const std::string& room = presence.room.name;
    switch (presence.status) {
    case nf::Status::Fighting: if (presence.table >= 0) return loc::Tf("friends.playing_table", presence.table + 1, room); break;
    case nf::Status::Watching: if (presence.table >= 0) return loc::Tf("friends.watching_table", presence.table + 1, room); break;
    case nf::Status::Training: return loc::Tf("friends.training_room", room);
    default: break;
    }
    return loc::Tf("friends.waiting_room", room);
}

std::string PlayedLine(const nf::Incoming& request, std::uint64_t now) {
    if (!request.games) return loc::T("friends.not_played");
    const std::string ago = Ago(now, request.lastGameAt);
    return loc::Tf("friends.played_you", request.games, ago.empty() ? std::string("-") : ago);
}

std::size_t CodeSymbols(const std::string& typed) {
    return static_cast<std::size_t>(std::count_if(typed.begin(), typed.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }));
}
std::string CodeShown(const std::string& typed) {
    std::string symbols;
    for (const char c : typed) if (std::isalnum(static_cast<unsigned char>(c))) symbols += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return symbols.size() > 4 ? symbols.substr(0, 4) + "-" + symbols.substr(4) : symbols;
}

std::vector<MenuChoice> FriendChoices(const nf::State& state, const nf::Friend& entry, bool canInvite, Gate gate) {
    const auto reason = [](const char* key) { return key ? std::string(loc::T(key)) : std::string(); };
    const auto join = nf::ToFriend(state, entry.id, false), watch = nf::ToFriend(state, entry.id, true);
    const char* joinWhy = !gate.go ? gate.goWhy : WhyNotThere(join, false);
    const char* watchWhy = !gate.go ? gate.goWhy : WhyNotThere(watch, true);
    const bool online = entry.presence.kind != nf::Presence::Kind::Offline;
    const char* inviteWhy = !gate.act ? gate.actWhy : !online ? "friends.invite_unavailable" : !canInvite ? "friends.nothing_to_offer" : nullptr;
    // Inviting while appearing offline tells the friend where this player is.
    const std::string inviteDetail = inviteWhy ? reason(inviteWhy) : state.appearOffline ? reason("friends.invite_reveals") : std::string();
    return {Choice("join", "friends.join_room", !joinWhy, reason(joinWhy)), Choice("watch", "friends.watch", !watchWhy, reason(watchWhy)),
        Choice("invite", "friends.invite_room", !inviteWhy, inviteDetail), Choice("remove", "friends.remove", gate.act, reason(gate.actWhy)),
        Choice("remove-block", "friends.remove_block", gate.act, reason(gate.actWhy))};
}

std::vector<MenuChoice> RequestChoices(Gate gate) {
    const std::string why = gate.act ? std::string() : std::string(loc::T(gate.actWhy));
    return {Choice("accept", "friends.accept", gate.act, why), Choice("decline", "friends.decline", gate.act, why),
        Choice("decline-block", "friends.decline_block", gate.act, why)};
}

std::vector<MenuChoice> AskChoices(const std::string& ask, const std::string& name) {
    const char* label = ask == "remove" ? "friends.remove" : ask == "remove-block" ? "friends.remove_block" : ask == "decline" ? "friends.decline" : "friends.decline_block";
    return {Choice("yes", label, true, name)};
}

Outcome OutcomeOf(const nf::View& view, std::uint64_t finished, std::uint64_t refusals, double deadline, double now) {
    if (view.refusals > refusals) return Outcome::Refused;
    if (view.state.finished > finished && !view.busy) return StateOf(view).kind == nf::State::Kind::Off ? Outcome::Off : Outcome::Done;
    return now > deadline ? Outcome::GivenUp : Outcome::Waiting;
}

std::string AcceptedWords(const nf::State& state, const std::string& request) {
    const auto found = std::find_if(state.incoming.begin(), state.incoming.end(), [&](const nf::Incoming& r) { return r.id == request; });
    return found == state.incoming.end() ? std::string() : loc::Tf("friends.now_friends", found->name);
}
}

namespace {
using namespace friends_panel;

// A friends command made for the room the player saw carries its generation.
template <class Command, class = void> struct HasGenerationOf : std::false_type {};
template <class Command> struct HasGenerationOf<Command, std::void_t<decltype(std::declval<Command&>().generation)>> : std::true_type {};
template <class Command> constexpr bool HasGeneration = HasGenerationOf<Command>::value;

MenuEntry NoteRowOf(const char* words) {
    auto row = InfoRow(NoteRow, loc::T(words), {}, {});
    row.quiet = true;
    return row;
}

MenuEntry RequestRowOf(const nf::Incoming& request, std::uint64_t now) {
    auto row = Row(RequestPrefix + request.id, request.name, PlayedLine(request, now));
    row.userText = true;
    row.value = loc::T("friends.accept");
    row.valueHighlighted = true;
    row.hint = loc::T("friends.accept");
    return row;
}

MenuEntry FriendRowOf(const nf::Friend& entry, bool canInvite, std::uint64_t now) {
    auto row = Row(FriendPrefix + entry.id, entry.name, Status(entry, now));
    row.userText = true;
    row.detailText = DetailText::Name;
    switch (PressOf(entry, canInvite)) {
    case Press::Join: row.value = row.hint = loc::T("friends.join"); break;
    case Press::Invite: row.value = row.hint = loc::T("friends.invite"); break;
    case Press::More: row.hint = loc::T("friends.more"); break;
    }
    return row;
}

// Turn on friends, for a player who turned them off: asks for nothing the bridge refuses while another action is in flight.
MenuEntry TurnOnRowOf(const nf::View& view, bool pending) {
    const bool free = !view.busy && !pending;
    auto row = Row(TurnOnRow, loc::T("friends.turn_on"), loc::T(free ? "friends.turn_on_detail" : "friends.busy"), free);
    row.hint = loc::T("friends.turn_on");
    return row;
}

std::string DateOf(std::uint64_t at) { return at ? LocalTime(at, "%Y-%m-%d") : std::string("-"); }

// The words that say why the friends cannot be used now, or null when they can: the state's own, or the gate's.
const char* UnusableWords(Mode mode, Gate gate) {
    const char* words = ModeWords(mode);
    return words ? words : gate.actWhy;
}
}

bool FriendsPanel::SettingsRow(const std::string& row) { return row == AppearRow || row == NewCodeRow || row == ForgetRow || row == TurnOnRow; }

std::vector<MenuEntry> FriendsPanel::SettingsRows(const ShellView& v) {
    const auto& f = v.friends;
    const auto& state = StateOf(f);
    const Gate gate = GateOf(f, service_, pending_.active);
    const char* why = UnusableWords(ModeOf(f, service_), gate);
    const auto detail = [&](const char* words) { return why ? std::string(loc::T(why)) : std::string(loc::T(words)); };
    auto appear = Row(AppearRow, loc::T("friends.appear_offline"), detail("friends.appear_offline_detail"), !why);
    appear.value = loc::T(state.appearOffline ? "common.on" : "common.off");
    auto code = ConfirmRow(NewCodeRow, loc::T("friends.code"), detail("friends.new_code_detail"), !why);
    code.value = state.code.empty() ? std::string("-") : state.code;
    code.wholeValue = true;
    // Turned off, the one way on is Turn on: the other rows say why they wait.
    auto off = ModeOf(f, service_) == Mode::Off ? TurnOnRowOf(f, pending_.active) :
        ConfirmRow(ForgetRow, loc::T("friends.turn_off"), detail("friends.turn_off_detail"), !why);
    for (auto* row : {&appear, &code, &off}) if (pending_.active && pending_.row == row->id) row->value = "...";
    return {appear, code, off};
}

void FriendsPanel::RequestMember(const ShellView& v, const std::string& card, const std::string& name, bool accepts, const Submit& submit) {
    const Gate gate = MemberGateOf(v.friends, service_, pending_.active);
    if (!gate.act) { Say(loc::T(gate.actWhy), Tone::Error); return; }
    Send(v, commands::FriendRequest{Bridge(v), card, name}, submit, {}, loc::Tf(accepts ? "friends.now_friends" : "friends.request_sent_to", name));
}

void FriendsPanel::BlockMember(const ShellView& v, const std::string& card, const std::string& name, const Submit& submit) {
    const Gate gate = MemberGateOf(v.friends, service_, pending_.active);
    if (!gate.act) { Say(loc::T(gate.actWhy), Tone::Error); return; }
    Send(v, commands::FriendBlock{Bridge(v), card, name}, submit, {}, loc::Tf("friends.blocked_name", name));
}

bool FriendsPanel::AnswerToast(const ShellView& v, const RuntimeRequest& command, const Submit& submit) {
    const auto* accept = std::get_if<commands::FriendAccept>(&command);
    if (!accept) return false;
    const Gate gate = GateOf(v.friends, service_, pending_.active);
    if (!gate.act) { Say(loc::T(gate.actWhy), Tone::Error); return true; }
    Send(v, *accept, submit, RequestPrefix + accept->request, AcceptedWords(StateOf(v.friends), accept->request));
    return true;
}

const std::string& FriendsPanel::Bridge(const ShellView& view) const { return view.friends.bridge.empty() ? bridge_ : view.friends.bridge; }

void FriendsPanel::Say(std::string words, Tone tone) {
    said_ = std::move(words);
    saidTone_ = tone;
    hasSaid_ = !said_.empty();
    again_ = false;
}

bool FriendsPanel::TakeSaid(std::string& words, Tone& tone) {
    if (!hasSaid_) return false;
    words = std::move(said_);
    tone = saidTone_;
    said_.clear();
    hasSaid_ = false;
    // Shown from now as a notice's card; said a second time, they are not said a third.
    if (again_) spoken_.reset();
    else spoken_ = Spoken{words, tone, Told{now_, ToastSeconds}};
    again_ = false;
    return true;
}

template <class Command>
bool FriendsPanel::Send(const ShellView& view, Command command, const Submit& submit, const std::string& row, std::string say, bool back) {
    // Going to a friend is made for the room the player saw.
    if constexpr (HasGeneration<Command>) command.generation = view.session.generation;
    if (command.bridge.empty() || !submit(std::move(command))) {
        Say(loc::T("error.queue_failed"), Tone::Error);
        return false;
    }
    // Only the bridge's actions wait on an answer.
    if constexpr (commands::IsFriendAction<Command>)
        pending_ = Pending{true, row, std::move(say), view.friends.state.finished, view.friends.refusals, now_ + GiveUpSeconds, back};
    return true;
}

void FriendsPanel::Update(const ShellView& v, MenuNavigation& nav, const std::string& bridge, double now, const Submit& submit) {
    const auto& f = v.friends;
    now_ = now;
    bridge_ = bridge;
    service_ = !bridge.empty();
    if (!started_) { started_ = true; refusalsSeen_ = f.refusals; visitHandled_ = f.visit.revision; }
    const auto quiet = [&](auto command) { return !command.bridge.empty() && submit(std::move(command)); };
    // The cadence: Visit as the tab opens, again when it lapsed while shown; Leave as it closes.
    if (Shown(nav.Screen())) {
        if (!asked_ || (sawVisiting_ && !f.visiting)) {
            if (quiet(commands::FriendsVisit{Bridge(v)})) { asked_ = true; sawVisiting_ = false; }
        } else if (f.visiting) sawVisiting_ = true;
    } else if (asked_) {
        if (Bridge(v).empty() || quiet(commands::FriendsLeave{Bridge(v)})) { asked_ = false; sawVisiting_ = false; }
    }
    // What was asked went through, or was refused, or had no answer: each says so.
    if (pending_.active) {
        switch (OutcomeOf(f, pending_.finished, pending_.refusals, pending_.deadline, now)) {
        case Outcome::Waiting: break;
        // The refusal's own words are said below.
        case Outcome::Refused: pending_.active = false; break;
        case Outcome::Done:
            if (!pending_.say.empty()) Say(pending_.say, Tone::Success);
            if (pending_.back && nav.Screen() == AddScreen) nav.Return();
            pending_.active = false;
            break;
        case Outcome::Off:
            // Friends found off is a state: Turn off ends by it and says nothing more; anything else was not done.
            if (pending_.row != ForgetRow) Say(loc::T("friends.off"), Tone::Error);
            pending_.active = false;
            break;
        case Outcome::GivenUp:
            Say(loc::T("friends.failed"), Tone::Error);
            pending_.active = false;
            break;
        }
    }
    if (f.refusals != refusalsSeen_) {
        refusalsSeen_ = f.refusals;
        if (!f.refusalText.empty()) Say(f.refusalText, Tone::Error);
    }
    // Words Ember hid before they could be read are said once more, on the next free menu.
    const bool free = nav.Screen() != PromptScreen && nav.Mode() == MenuNavigation::Modal::None;
    if (spoken_ && spoken_->told.unseen && free && !hasSaid_) {
        Say(spoken_->words, spoken_->tone);
        again_ = true;
    }
    if (nav.Screen() != AddScreen && !pending_.active) code_.clear();
    // A Join or Watch that waits on the player, over whatever shows; it ends with the question.
    const bool asking = f.visit.active && f.visit.confirm;
    if (asking && f.visit.revision != visitHandled_ && nav.Screen() != VisitScreen) { visitHandled_ = f.visit.revision; nav.Push(VisitScreen); }
    else if (!asking && nav.Screen() == VisitScreen) nav.Return();
    Announce(v, nav.Screen(), free);
}

bool FriendsPanel::TakeToast(ToastRequest& toast) {
    if (!toast_) return false;
    toast = std::move(*toast_);
    toast_.reset();
    return true;
}

void FriendsPanel::Announce(const ShellView& v, const std::string& screen, bool free) {
    const auto& state = StateOf(v.friends);
    if (state.kind != nf::State::Kind::Listed || v.friends.bridge.empty()) return;
    // What is no longer in the state is forgotten, so what this remembers stays as small as the lists are.
    for (auto id = told_.begin(); id != told_.end();) {
        const bool kept = std::any_of(state.incoming.begin(), state.incoming.end(), [&](const nf::Incoming& r) { return r.id == *id; }) ||
            std::any_of(state.invites.begin(), state.invites.end(), [&](const nf::Invite& i) { return i.id == *id; });
        id = kept ? std::next(id) : told_.erase(id);
    }
    // A toast is followed until it was seen to its end, it is gone from the state, or the Friends screens list it.
    const bool shown = Shown(screen);
    for (auto toasted = toasted_.begin(); toasted != toasted_.end();)
        toasted = !told_.count(toasted->first) || toasted->second.told.Lapsed(now_) || shown ? toasted_.erase(toasted) : std::next(toasted);
    const bool quiet = !read_ || shown;
    read_ = true;
    const std::string& bridge = v.friends.bridge;
    // The toast of the request or invite `id` while it stands, with its buttons.
    const auto toastOf = [&](const std::string& id) -> std::optional<ToastRequest> {
        ToastRequest toast;
        toast.tone = Tone::Pending;
        toast.seconds = ActionToastSeconds;
        for (const auto& request : state.incoming) {
            if (request.id != id) continue;
            toast.text = loc::Tf("friends.toast_request", request.name) + "\n" + PlayedLine(request, v.unixNow);
            toast.actions = {{loc::T("friends.accept"), "", "", {commands::FriendAccept{bridge, request.id}}}, {loc::T("friends.later")}};
            return toast;
        }
        for (const auto& invite : state.invites) {
            if (invite.id != id || (invite.expiresAt && v.unixNow >= invite.expiresAt)) continue;
            const auto* from = state.FindFriend(invite.friendId);
            if (!from) return std::nullopt;
            // A public room is named by the friend who is in it.
            const auto& at = from->presence;
            const bool named = invite.room.kind == nf::InviteRoom::Kind::RoomId && at.hasRoom && at.room.id == invite.room.value;
            toast.text = loc::Tf("friends.toast_invite", from->name) + (named ? "\n" + at.room.name : std::string());
            toast.actions = {{loc::T("toast.join"), "", "", {commands::FriendTakeInvite{v.session.generation, bridge, invite.id}, commands::FriendDismiss{bridge, invite.id}}},
                {loc::T("friends.dismiss"), "", "", {commands::FriendDismiss{bridge, invite.id}}}};
            return toast;
        }
        return std::nullopt;
    };
    // One said where it could not be seen or answered by key is said once more, now the menu is free, and then let go.
    if (free) {
        const auto unseen = std::find_if(toasted_.begin(), toasted_.end(), [](const auto& t) { return t.second.told.unseen; });
        if (unseen != toasted_.end()) {
            auto toast = toastOf(unseen->first);
            toasted_.erase(unseen);
            if (toast) { toast_ = std::move(*toast); return; }
        }
    }
    // A new one is remembered whether it is said or not; one said is one a frame, so a second waits for the next.
    const auto tell = [&](const std::string& id) {
        auto toast = toastOf(id);
        if (!toast) return false;
        // Over the post-match prompt its keys answer the prompt, so it is not seen through there.
        toasted_[id] = Toasted{Told{now_, ActionToastSeconds, screen == PromptScreen}, toast->text};
        toast_ = std::move(*toast);
        return true;
    };
    for (const auto& request : state.incoming)
        if (told_.insert(request.id).second && !quiet && tell(request.id)) return;
    for (const auto& invite : state.invites)
        if (told_.insert(invite.id).second && !quiet && tell(invite.id)) return;
}

void FriendsPanel::ToastAnswered(const std::string& text) {
    for (auto toasted = toasted_.begin(); toasted != toasted_.end();)
        toasted = toasted->second.text == text ? toasted_.erase(toasted) : std::next(toasted);
}

void FriendsPanel::Conceal() {
    asked_ = sawVisiting_ = false;
    raise_ = Raise{};
    // Hidden before their card's time was up, words and toasts are said again on the next free menu.
    if (spoken_) {
        spoken_->told.Hidden(now_);
        if (spoken_->told.Lapsed(now_)) spoken_.reset();
    }
    for (auto& toasted : toasted_) toasted.second.told.Hidden(now_);
}

std::vector<MenuEntry> FriendsPanel::Rows(const ShellView& v, const std::string& screen, bool dialog, std::string& title) {
    const auto& f = v.friends;
    const auto& state = StateOf(f);
    const Mode mode = ModeOf(f, service_);
    const Gate gate = GateOf(f, service_, pending_.active);
    const std::uint64_t now = v.unixNow;
    // A popout waits on a row of the list: with another screen or no list, it is gone.
    if (screen != Screen || mode != Mode::Listed) raise_ = Raise{};
    const auto pendingOn = [&](MenuEntry& row) { if (pending_.active && pending_.row == row.id) row.value = "..."; };
    std::vector<MenuEntry> rows;
    // The question a Join asks before it leaves a room or a game.
    if (screen == VisitScreen) {
        title = loc::T("friends.visit_title");
        const std::string name = f.visit.name;
        auto stay = Row(StayRow, loc::T("friends.visit_stay"), loc::T("friends.visit_stay_detail"));
        stay.hint = loc::T("friends.visit_stay");
        auto go = ConfirmRow(GoRow, loc::Tf(f.visit.watch ? "friends.visit_watch" : "friends.visit_join", name), loc::T("friends.visit_detail"));
        go.detailText = DetailText::Name;
        go.userText = true;
        return {stay, go};
    }
    if (screen == AddScreen) {
        title = loc::T("friends.add");
        auto code = InfoRow("friends-code", loc::T("friends.code"), state.code, loc::T("friends.code_detail"));
        auto fresh = ConfirmRow(NewCodeRow, loc::T("friends.new_code"), loc::T("friends.new_code_detail"), gate.act);
        if (!gate.act) fresh.detail = loc::T(gate.actWhy);
        pendingOn(fresh);
        auto their = TextRow(TheirCodeRow, loc::T("friends.their_code"), code_, 9, gate.act);
        auto send = Row(SendRow, loc::T("friends.send"), loc::T("friends.send_detail"), gate.act && CodeSymbols(code_) == CodeLength);
        send.hint = loc::T("friends.send");
        pendingOn(send);
        return {code, fresh, their, send};
    }
    if (screen == SentScreen || screen == BlockedScreen) {
        const bool sent = screen == SentScreen;
        title = loc::Tf(sent ? "friends.sent" : "friends.blocked", sent ? state.outgoing.size() : state.blocked.size());
        const auto add = [&](std::string id, const std::string& name, std::string value, const char* choice, const char* word) {
            auto row = Row(std::move(id), name.empty() ? std::string("-") : name, {});
            row.userText = true;
            row.value = std::move(value);
            row.choices = {{choice, loc::T(word), name, gate.act}};
            if (!gate.act) row.choices[0].detail = loc::T(gate.actWhy);
            pendingOn(row);
            rows.push_back(std::move(row));
        };
        if (sent) for (const auto& o : state.outgoing) add(SentPrefix + o.id, o.label, loc::Tf("friends.sent_ago", Ago(now, o.sentAt).empty() ? DateOf(o.sentAt) : Ago(now, o.sentAt)), "cancel", "friends.cancel_request");
        else for (const auto& b : state.blocked) add(BlockedPrefix + b.id, b.name, DateOf(b.at), "unblock", "friends.unblock");
        if (rows.empty()) rows.push_back(InfoRow(NoteRow, title, {}, {}));
        return rows;
    }
    // The tab.
    title = loc::T("profile.tab.friends");
    switch (mode) {
    case Mode::NoAccount: {
        // The one way on: Connect Discord, focused.
        auto connect = ConnectDiscordRow();
        connect.hint = loc::T("screen.connect_discord");
        return {connect};
    }
    case Mode::Off: return {NoteRowOf("friends.off"), TurnOnRowOf(f, pending_.active)};
    case Mode::Unavailable: return {NoteRowOf("friends.unavailable")};
    case Mode::Unreachable: return {NoteRowOf("friends.unreachable")};
    case Mode::Loading: {
        auto add = Row(AddRow, loc::T("friends.add"), loc::T("friends.code_detail"), false);
        rows = {add, NoteRowOf("friends.loading")};
        return rows;
    }
    case Mode::Listed: break;
    }
    const auto groups = Arrange(state);
    auto add = Row(AddRow, loc::T("friends.add"), loc::T("friends.code_detail"));
    add.value = loc::Tf("friends.your_code", state.code);
    add.hint = loc::T("friends.open");
    rows.push_back(std::move(add));
    const auto group = [&](const char* key, std::size_t count, MenuEntry& row) { row.group = loc::Tf(key, count); };
    const auto first = rows.size();
    for (const auto* request : groups.requests) {
        auto row = RequestRowOf(*request, now);
        group("friends.requests_heading", groups.requests.size(), row);
        pendingOn(row);
        rows.push_back(std::move(row));
    }
    const auto friendsIn = [&](const std::vector<const nf::Friend*>& list, const char* key) {
        for (const auto* entry : list) {
            auto row = FriendRowOf(*entry, f.canInvite, now);
            group(key, list.size(), row);
            pendingOn(row);
            rows.push_back(std::move(row));
        }
    };
    friendsIn(groups.inRoom, "friends.in_room");
    friendsIn(groups.online, "friends.online");
    friendsIn(groups.offline, "friends.offline");
    if (rows.size() == first) rows.push_back(NoteRowOf("friends.empty"));
    if (!state.outgoing.empty()) {
        auto sent = Row(SentRow, loc::Tf("friends.sent", state.outgoing.size()), {});
        sent.hint = loc::T("friends.open");
        rows.push_back(std::move(sent));
    }
    if (!state.blocked.empty()) {
        auto blocked = Row(BlockedRow, loc::Tf("friends.blocked", state.blocked.size()), {});
        blocked.hint = loc::T("friends.open");
        rows.push_back(std::move(blocked));
    }
    // A switch Select flips, which is an action here (not an adjustable value, which the shell's panels cannot send).
    auto appear = Row(AppearRow, loc::T("friends.appear_offline"), gate.act ? loc::T("friends.appear_offline_detail") : loc::T(gate.actWhy), gate.act);
    appear.value = loc::T(state.appearOffline ? "common.on" : "common.off");
    appear.hint = loc::T("friends.change");
    pendingOn(appear);
    rows.push_back(std::move(appear));
    Attach(rows, v, dialog);
    return rows;
}

// The popout the player asked for (More, or the question that asks again) goes on its row, which Dress then opens.
void FriendsPanel::Attach(std::vector<MenuEntry>& rows, const ShellView& v, bool dialog) {
    if (raise_.row.empty()) return;
    if (raise_.issued) {
        if (dialog) raise_.seen = true;
        else if (raise_.seen || ++raise_.waits > 3) { raise_ = Raise{}; return; }
    } else if (++raise_.waits > 8) { raise_ = Raise{}; return; }
    const auto row = std::find_if(rows.begin(), rows.end(), [&](const MenuEntry& e) { return e.id == raise_.row; });
    if (row == rows.end()) { raise_ = Raise{}; return; }
    const auto& f = v.friends;
    const auto& state = StateOf(f);
    const Gate gate = GateOf(f, service_, pending_.active);
    if (const auto* entry = state.FindFriend(IdOf(row->id, FriendPrefix)); entry && HasPrefix(row->id, FriendPrefix)) {
        row->choices = raise_.ask.empty() ? FriendChoices(state, *entry, f.canInvite, gate) : AskChoices(raise_.ask, entry->name);
    } else if (HasPrefix(row->id, RequestPrefix)) {
        const std::string id = IdOf(row->id, RequestPrefix);
        const auto request = std::find_if(state.incoming.begin(), state.incoming.end(), [&](const nf::Incoming& r) { return r.id == id; });
        if (request == state.incoming.end()) { raise_ = Raise{}; return; }
        row->choices = raise_.ask.empty() ? RequestChoices(gate) : AskChoices(raise_.ask, request->name);
    } else { raise_ = Raise{}; return; }
    row->popout = true;
}

bool FriendsPanel::Activate(const ShellView& v, const MenuAction& a, MenuNavigation& nav, const Submit& submit) {
    const auto& f = v.friends;
    const auto& state = StateOf(f);
    const Gate gate = GateOf(f, service_, pending_.active);
    const std::string& bridge = Bridge(v);
    const auto refuse = [&](const char* why) { if (why) Say(loc::T(why), Tone::Error); };
    if (a.id == AddRow || a.id == SentRow || a.id == BlockedRow) { nav.Push(a.id); return true; }
    if (a.id == StayRow || a.id == GoRow) { Send(v, commands::FriendAnswerVisit{{}, bridge, f.visit.revision, a.id == GoRow}, submit); return true; }
    if (a.id == AppearRow) {
        if (!gate.act) refuse(gate.actWhy);
        else Send(v, commands::FriendAppearOffline{bridge, !state.appearOffline}, submit, a.id);
        return true;
    }
    if (a.id == NewCodeRow) {
        if (!gate.act) refuse(gate.actWhy);
        else Send(v, commands::FriendNewCode{bridge}, submit, a.id);
        return true;
    }
    if (a.id == TurnOnRow) {
        if (f.busy || pending_.active) refuse("friends.busy");
        else Send(v, commands::FriendTurnOn{bridge}, submit, a.id, loc::T("friends.turned_on"));
        return true;
    }
    if (a.id == ForgetRow) {
        if (!gate.act) refuse(gate.actWhy);
        else Send(v, commands::FriendTurnOff{bridge}, submit, a.id);
        return true;
    }
    if (a.id == SendRow) {
        if (!gate.act) refuse(gate.actWhy);
        else Send(v, commands::FriendRequestByCode{bridge, code_}, submit, a.id, loc::Tf("friends.request_sent_to", CodeShown(code_)), true);
        return true;
    }
    if (HasPrefix(a.id, RequestPrefix)) {
        const std::string id = IdOf(a.id, RequestPrefix);
        if (!gate.act) refuse(gate.actWhy);
        else Send(v, commands::FriendAccept{bridge, id}, submit, a.id, AcceptedWords(state, id));
        return true;
    }
    if (HasPrefix(a.id, FriendPrefix)) {
        const auto* entry = state.FindFriend(IdOf(a.id, FriendPrefix));
        if (!entry) return true;
        switch (PressOf(*entry, f.canInvite)) {
        case Press::Join:
            if (!gate.go) refuse(gate.goWhy);
            else Send(v, commands::FriendJoin{{}, bridge, entry->id}, submit);
            break;
        case Press::Invite:
            if (!gate.act) refuse(gate.actWhy);
            else Send(v, commands::FriendInvite{bridge, entry->id}, submit, a.id, loc::Tf("friends.invite_sent", entry->name));
            break;
        case Press::More: Ask(a.id, {}); break;
        }
        return true;
    }
    return false;
}

bool FriendsPanel::Choose(const ShellView& v, const MenuAction& a, const Submit& submit) {
    const auto& f = v.friends;
    const auto& state = StateOf(f);
    const Gate gate = GateOf(f, service_, pending_.active);
    const std::string& bridge = Bridge(v);
    // A page's one choice: take the request back, lift the block.
    if (HasPrefix(a.id, SentPrefix) || HasPrefix(a.id, BlockedPrefix)) {
        if (!gate.act) { Say(loc::T(gate.actWhy), Tone::Error); return true; }
        if (a.text == "cancel") Send(v, commands::FriendCancel{bridge, IdOf(a.id, SentPrefix)}, submit, a.id);
        else if (a.text == "unblock") Send(v, commands::FriendUnblock{bridge, IdOf(a.id, BlockedPrefix)}, submit, a.id);
        return true;
    }
    if (raise_.row != a.id) return true;
    const std::string ask = raise_.ask;
    raise_ = Raise{};
    const bool request = HasPrefix(a.id, RequestPrefix);
    const std::string id = IdOf(a.id, request ? RequestPrefix : FriendPrefix);
    // The question answered: now do what it asked about.
    const std::string& what = ask.empty() ? a.text : ask;
    if (!ask.empty() && a.text != "yes") return true;
    // More's choices that ask first go on to the question.
    if (ask.empty() && (what == "remove" || what == "remove-block" || what == "decline" || what == "decline-block")) { Ask(a.id, what); return true; }
    if (what == "join" || what == "watch") {
        if (!gate.go) Say(loc::T(gate.goWhy), Tone::Error);
        else if (what == "join") Send(v, commands::FriendJoin{{}, bridge, id}, submit);
        else Send(v, commands::FriendWatch{{}, bridge, id}, submit);
        return true;
    }
    if (!gate.act) { Say(loc::T(gate.actWhy), Tone::Error); return true; }
    if (what == "accept") Send(v, commands::FriendAccept{bridge, id}, submit, a.id, AcceptedWords(state, id));
    else if (what == "invite") {
        const auto* entry = state.FindFriend(id);
        Send(v, commands::FriendInvite{bridge, id}, submit, a.id, entry ? loc::Tf("friends.invite_sent", entry->name) : std::string());
    }
    else if (what == "remove" || what == "remove-block") Send(v, commands::FriendRemove{bridge, id, what == "remove-block"}, submit, a.id);
    else if (what == "decline" || what == "decline-block") Send(v, commands::FriendDecline{bridge, id, what == "decline-block"}, submit, a.id);
    return true;
}

bool FriendsPanel::Adjust(const MenuAction& a) {
    if (a.id != TheirCodeRow || a.kind != MenuAction::TextAccepted) return false;
    code_ = a.text;
    return true;
}

void FriendsPanel::Shortcut(const ShellView& v, const MenuAction& a, MenuNavigation& nav) {
    if (nav.Screen() != Screen) return;
    const std::string& focus = nav.Focus();
    if (a.delta == MenuInput::Fighter) { if (ModeOf(v.friends, service_) == Mode::Listed) nav.Push(AddScreen); }
    else if (a.delta == MenuInput::Options) { if (HasPrefix(focus, RequestPrefix) || HasPrefix(focus, FriendPrefix)) Ask(focus, {}); }
    else if (a.delta == MenuInput::Leave) { if (HasPrefix(focus, FriendPrefix)) Ask(focus, "remove"); }
}

void FriendsPanel::Dress(const ShellView& v, const std::string& screen, const std::vector<MenuEntry>& rows, GameMenu& menu) {
    const auto& f = v.friends;
    const auto& colors = v2::Colors();
    if (Shown(screen)) {
        if (StateOf(f).appearOffline) menu.headerChips.push_back({loc::T("friends.appearing_offline"), colors.muted});
        if (Stale(f)) menu.headerChips.push_back({loc::T("friends.not_updated"), colors.muted});
    }
    // X and Y are the tab's here, in a room too (ApplicationShell takes them before the room's own).
    if (screen == Screen && ModeOf(f, service_) == Mode::Listed) {
        const std::string& focus = menu.navigation.Focus();
        std::vector<std::pair<unsigned, std::string>> hints;
        if (HasPrefix(focus, RequestPrefix) || HasPrefix(focus, FriendPrefix)) hints.push_back({MenuInput::Options, loc::T("friends.more")});
        hints.push_back({MenuInput::Fighter, loc::T("friends.add")});
        menu.shortcutHints = ShortcutHints(hints);
    }
    if (!raise_.row.empty() && !raise_.issued && menu.navigation.Ask(raise_.row, rows)) { raise_.issued = true; raise_.seen = false; raise_.waits = 0; }
}
} }
