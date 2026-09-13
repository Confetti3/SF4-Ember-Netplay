#pragma once
#include <string>
namespace sf4e { namespace ui {
// Return true only for an explicit launch retry. Network work stays on the
// application worker; no game is created by this presentation surface.
bool RunRecovery(std::string message, std::wstring& gameDirectory, bool updates = false);
} }
