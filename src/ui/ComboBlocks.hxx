#pragma once
#include "MenuNavigation.hxx"
#include <string>
#include <vector>

// The pattern editor: a combo's moves as blocks on a frame ruler, laid out
// the way a step sequencer lays out notes. Each block is one step with a
// "#frame"; moving a block rewrites that frame, and the replay plays the
// pattern on those frames.
namespace sf4e { namespace ui {
// Rows the pattern screen navigates: one adjustable row per move, then the
// add, delete and replay rows. Up and Down pick a block, Left and Right nudge it.
std::vector<MenuEntry> PatternRows(const std::vector<std::string>& steps);
// Gives every move without a frame one, 20 frames after the move before it.
bool LayOutPattern(std::vector<std::string>& steps);
// Moves a block by delta frames. Returns whether anything changed.
bool NudgePattern(std::vector<std::string>& steps, std::size_t index, int delta);
// Adds the moves of a typed line after the last block.
bool AddToPattern(std::vector<std::string>& steps, const std::string& line, const std::string& fighter, std::string& error);
// Draws the ruler, the blocks and the action rows in place of the menu list;
// the mouse may drag a block. Returns whether it changed the steps.
bool DrawPattern(std::vector<std::string>& steps, const std::vector<MenuEntry>& entries, MenuNavigation& nav, float height, float unit);
// The block the player last had focused, for the delete row.
std::size_t FocusedPatternBlock();
} }
