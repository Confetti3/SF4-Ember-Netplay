#pragma once
#include <imgui.h>
#include <string>
#include <vector>

namespace sf4e { namespace ui {
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
// The height DrawDialogButtons will take here, for dialogs that lay out around it.
float DialogButtonsHeight(const std::vector<DialogButton>& buttons,float height);
// The panel of a dialog drawn in place rather than as a popup (the flyout's
// confirmation, a button capture's card): a bordered child of `size` at `at`,
// in screen coordinates. Pair with EndDialogCard.
void BeginDialogCard(const char* id,ImVec2 at,ImVec2 size);
void EndDialogCard();
// The columns a choice of `count` options is drawn in: one in the classic
// look; in V2 all options show at once, up to four to a row.
std::size_t MenuChoiceColumns(std::size_t count);
} }
