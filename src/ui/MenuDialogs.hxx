#pragma once
#include <imgui.h>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
struct MenuChoice;
// Elides text to width with "...". Text probes measure the original label,
// never this result, so an ellipsis cannot pass for a label that fits.
std::string FitLabel(const std::string& text,float width);
// enabled: the button can be clicked. lit: it looks available; a room
// checkpoint may dim a button that still works (MenuVisualFeedback).
struct DialogButton { std::string label; bool enabled=true, lit=true; };
// The buttons every dialog shares: side by side, or stacked when a label would
// not fit. *selected is highlighted and receives default focus on opening;
// mouse movement changes it only after the dialog appears. A resting pointer
// or keyboard focus cannot change it; nullptr highlights none. Set
// focusOnAppearing=false when a text editor owns the initial keyboard focus.
// Each label is reported to the text probe as "<probe>/<index>".
// Returns the clicked index, or -1.
int DrawDialogButtons(const char* probe,const std::vector<DialogButton>& buttons,int* selected,float height,bool focusOnAppearing=true);
// The panel of a dialog drawn in place rather than as a popup (a button
// capture's card): a bordered child of `size` at `at`,
// in screen coordinates. Pair with EndDialogCard.
void BeginDialogCard(const char* id,ImVec2 at,ImVec2 size);
void EndDialogCard();
// The columns a choice of `count` options is drawn in: all show at once,
// up to three to a row.
std::size_t MenuChoiceColumns(std::size_t count);
// The choice popup: its cells share one width that fits the longest label,
// up to MenuChoiceColumns across; when that many of that width do not fit,
// fewer, down to one column that Up and Down move through. The cells fit
// in 60% of the screen (720 px at most), and in `room` when it is given (a
// value panel's column on a board).
struct MenuChoiceGrid { std::size_t across; float cell; };
MenuChoiceGrid MenuChoiceLayout(const std::vector<MenuChoice>& choices, float room = 0);
} }
