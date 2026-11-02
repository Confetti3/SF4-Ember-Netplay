#pragma once
// The Random card shot of ui_render_test.cxx (docs/design/RANDOM_FIGHTER.md):
// the grid's last card focused, then saved, ruled in place of the fighter it
// rolled. Random is taken back after, so the shots that follow are as before.
#include "../ui/FighterSelector.hxx"
namespace {
template<class Draw>
void ShootRandomCard(sf4e::ui::FighterSelector& selector, bool& random, const Draw& draw) {
    using sf4e::ui::MenuInput;
    selector.Navigation().Home();selector.Navigation().Prefer("random-fighter");draw();
    Require(selector.Navigation().Focus()=="random-fighter","The grid has no Random card");
    draw("random-focus");
    draw(nullptr,MenuInput::Select,1);draw("random-saved");
    Require(random&&selector.Navigation().Screen()=="home","Select on the Random card did not save Random");
    random=false;draw();
}
}
