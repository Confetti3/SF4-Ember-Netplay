#pragma once
#include <cstdint>
#include "ContinuousChoices.hxx"

namespace sf4e { namespace continuousrematch {
// All decisions that can change native simulation belong in GGPO's saved
// timeline. Wall clocks, reports and connection observations do not.
struct State {
    unsigned victoryFrames=0;
    int resultFrame=-1;
    int choiceWinner=-1;
	std::uint32_t score[2]={0,0};
    continuouschoices::State choices;
};
constexpr std::uint32_t VictoryCallbackRvas[6]={0x1df8a0,0x1df700,0x1df4e0,0x1defe0,0x1de8b0,0x1dddc0};

} }
