#pragma once
// One line of passive text centred at the top of the game's picture: the
// overlay's hints, which take no input (OverlayLayers draws SF4's; a game
// whose overlay draws its own layers, as SFV Ember's does, calls them alone).

namespace sf4e { namespace ui {
// `text` `top` points down from the picture's top, at `alpha`, in an ImGui
// window named `name`.
void DrawPassiveHint(const char* name, const char* text, float top, float alpha = 1);
// "F10 or Start" (runtime.open_shortcut) at `alpha` (ShortcutHintAlpha);
// nothing at 0.
void DrawShortcutHint(float alpha);
} }
