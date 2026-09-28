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
// not fit. *selected is highlighted and follows a moving mouse, never a
// resting one; nullptr highlights none. Each label is reported to the text
// probe as "<probe>/<index>". Returns the clicked index, or -1.
int DrawDialogButtons(const char* probe,const std::vector<DialogButton>& buttons,int* selected,float height);
// The height DrawDialogButtons will take here, for dialogs that lay out around it.
float DialogButtonsHeight(const std::vector<DialogButton>& buttons,float height);
} }
