#pragma once
#include "TrainingSession.hxx"
#include <array>

// Where the runtime moves the fighters in the game for a Place, apart from
// the practice model so only the runtime and its test write native memory.
namespace sf4e { namespace training {
// The fighters' root positions as the game holds them ([0] is x), null
// where a fighter is not there.
using FighterRoots = std::array<float*, 2>;
// A Place: both fighters to their x, or neither when either is not there,
// and the session told where they stand at once, so the input of the update
// the Place came in already turns for them.
inline bool MoveFighters(Session& session, const FighterRoots& roots, const float (&x)[2]) {
    if (!roots[0] || !roots[1]) return false;
    roots[0][0] = x[0]; roots[1][0] = x[1];
    session.SetPositions(x[0], x[1]);
    return true;
}
} }
