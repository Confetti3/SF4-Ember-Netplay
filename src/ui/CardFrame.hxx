#pragma once
#include "PlayerCard.hxx"

namespace sf4e { namespace ui {
// The Full card as one frame draws it: a panel's Update fills it while its Card tab is on show, and the rows, the drawing
// and the opening focus read it. Nothing builds it on the spot: until Update has filled it, `built` is false and there is
// no card to read.
struct CardFrame {
    bool built = false;
    player_card::FullCardView view;
};
} }
