#include "PostMatchRows.hxx"
#include "GameMenu.hxx"
#include "LatencyMark.hxx"
#include "MenuProbes.hxx"
#include "PlayerCard.hxx"
#include "RoomPoints.hxx"
#include "ShellView.hxx"
#include "Theme.hxx"
#include "../common/Localization.hxx"
#include <algorithm>
#include <cfloat>
#include <cmath>

namespace sf4e { namespace ui { namespace post_match_rows {
namespace {
// The fighter a player shows for the game: the one picked in the room, else their main.
int FighterOf(const room::Member& member) { return member.fighter >= 0 ? member.fighter : member.mainFighter; }

// The player's picture in `tile` with its tier line, or a bare frame without a fighter.
void DrawPicture(const ShellView& v, ImDrawList* d, v2::Box tile, const room::Member* member) {
    const auto& c = v2::Colors();
    const int fighter = member ? FighterOf(*member) : -1;
    if (fighter < 0) { d->AddRectFilled(tile.min, tile.max, c.frame); return; }
    const FighterPicture picture{fighter, v.preferences.roomCharacterPictures ? FighterArt::Character : FighterArt::Portrait, member->costume, member->color};
    DrawCharacterPortrait(picture, tile.min, tile.max, c.frame);
    if (const auto points = PointsOf(v, *member, fighter)) DrawFighterEdge(d, tile, points->points, points->tier);
}
}

v2::Box ButtonBox(v2::Box row) {
    // As wide as the widest of its four words in this language needs (the button's 17 px words and 12 px each side), so no state
    // cuts its label; at most half the row.
    const float s = Scale();
    float widest = ButtonWidth * s;
    for (const auto kind : {post_match_friend::State::Kind::Add, post_match_friend::State::Kind::Accept, post_match_friend::State::Kind::Sent,
             post_match_friend::State::Kind::Friends})
        widest = (std::max)(widest, std::ceil(HeadingFont()->CalcTextSizeA(17 * s, FLT_MAX, 0, loc::T(post_match_friend::LabelOf(kind))).x) + 24 * s);
    const float width = (std::min)(widest, row.W() * .5f);
    return {ImVec2(row.max.x - width, row.min.y), row.max};
}

std::optional<v2::Box> DrawPlayer(const ShellView& v, ImDrawList* d, v2::Box row, const Player& player, post_match_friend::State edge) {
    const float s = Scale();
    const auto& c = v2::Colors();
    d->AddRectFilled(row.min, row.max, c.ink, 4 * s);
    d->AddRect(row.min, row.max, c.border, 4 * s, 0, Hairline());
    const v2::Box button = ButtonBox(row);
    // The picture, then the tag, then the answer chip right after the name.
    const float side = row.H() - 16 * s;
    const v2::Box tile{ImVec2(row.min.x + 8 * s, row.min.y + 8 * s), ImVec2(row.min.x + 8 * s + side, row.min.y + 8 * s + side)};
    DrawPicture(v, d, tile, player.member);
    const float x = tile.max.x + 14 * s, right = edge.Shown() ? button.min.x - 12 * s : row.max.x - 12 * s;
    const float chip = v2::ChipWidth(player.word), line = (std::max)(ImGui::GetFontSize(), v2::ChipFont() + 8 * s);
    const float top = row.min.y + (row.H() - line) * .5f;
    float after = x;
    if (player.member) {
        const auto tag = MemberTag(v.memberLatency, *player.member, v.room.localMember);
        // The tag is given only the width it needs: Tag puts its ping at the far end of what it is given, and the chip follows that end.
        const float room = (std::min)((std::max)(right - x - chip - 12 * s, 60 * s), v2::TagWidth(tag, v2::TagSize::S));
        v2::Tag(d, ImVec2(x, top), room, tag, v2::TagSize::S);
        after = x + room + 12 * s;
    }
    const ImU32 tone = !player.seated ? c.danger : player.left ? c.muted : player.ready ? c.ready : ImGui::ColorConvertFloat4ToU32(ToneColor(Tone::Pending));
    v2::Chip(d, ImVec2(after, top), player.word, tone);
    ReportMenuCardText(player.id, player.word);
    if (!edge.Shown()) return std::nullopt;
    if (edge.Live()) return button;
    // Request sent, Friends: the same box, flat and muted, with its word and no glyph; nothing in it is focused.
    v2::Button(d, button, std::string(), v2::ButtonKind::Secondary, false, false, false);
    const std::string word = loc::T(post_match_friend::LabelOf(edge.kind));
    const float font = 17 * s, wide = (std::min)(button.W() - 24 * s, ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0, word.c_str()).x);
    v2::Text(d, ImVec2(button.min.x + (button.W() - wide) * .5f, button.min.y + (button.H() - font) * .5f), wide, word, font, c.muted);
    ReportMenuCardText(post_match_friend::RowId, word);
    return std::nullopt;
}

} } }
