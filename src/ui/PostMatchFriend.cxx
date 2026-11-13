#include "PostMatchFriend.hxx"
#include "MenuRows.hxx"
#include "RoomPoints.hxx"
#include "ShellView.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui { namespace post_match_friend {

State StateOf(const Inputs& in) {
    using Relation = netplay::levels::Relation;
    State state;
    if (!in.card) return state;
    switch (in.relation) {
    case Relation::Blocked: return state;
    case Relation::Friend: state.kind = State::Kind::Friends; break;
    case Relation::Sent: state.kind = State::Kind::Sent; break;
    // A request made here is as good as answered before the card says so: asked after, accepted when they had asked first.
    case Relation::Received: state.kind = in.asked ? State::Kind::Friends : State::Kind::Accept; break;
    case Relation::None: state.kind = in.asked ? State::Kind::Sent : State::Kind::Add; break;
    }
    return state;
}

Inputs InputsOf(const ShellView& view, const room::Member* opponent, bool asked) {
    Inputs in;
    in.asked = asked;
    if (!opponent || opponent->id == view.room.localMember || !opponent->card.Present()) return in;
    if (const auto* card = CardOf(view, *opponent)) {
        in.card = true;
        in.relation = card->relation;
    }
    return in;
}

const char* LabelOf(State::Kind kind) {
    switch (kind) {
    case State::Kind::Add: return "friends.add_member";
    case State::Kind::Accept: return "friends.accept_member";
    case State::Kind::Sent: return "friends.request_sent";
    case State::Kind::Friends: return "friends.are_friends";
    case State::Kind::None: break;
    }
    return nullptr;
}

const char* DetailOf(State::Kind kind) {
    switch (kind) {
    case State::Kind::Add: return "postmatch.add_friend.detail";
    case State::Kind::Accept: return "postmatch.accept_friend.detail";
    default: return nullptr;
    }
}

std::optional<MenuEntry> RowOf(State state, const std::string& opponentName) {
    if (!state.Live()) return std::nullopt;
    NoteUserText(opponentName);
    auto row = Row(RowId, loc::T(LabelOf(state.kind)), loc::Tf(DetailOf(state.kind), opponentName));
    row.detailText = DetailText::Name;
    row.wide = true;
    row.hint = loc::T(LabelOf(state.kind));
    return row;
}

} } }
