#pragma once
namespace sf4e { namespace ui {
class SelectionArt;
void DrawDeveloperOverlay(SelectionArt* art);
// In-game themes' picker, the developer page's body: also shown beside offline
// Training's controls, where Ember's menu does not open.
void DrawGameThemesPicker();
// F8 with Ember's menu closed, offline outside Training (whose controls have the picker): the picker in a
// small window, so the game's screen shows behind it as themes change. `here`: this is such a moment;
// `always`: shown without F8, beside Training's controls.
#ifdef SF4E_DEVELOPER_UI
void DrawGameThemesWindow(bool focused, bool here, bool always = false);
#else
inline void DrawGameThemesWindow(bool, bool, bool = false) {}
#endif
} }
