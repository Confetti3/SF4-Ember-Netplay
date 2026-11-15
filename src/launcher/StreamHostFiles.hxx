#pragma once
#include <string>

namespace sf4e { namespace launcher {
// Launcher.exe --write-stream-host: writes the stream PC's control panel (scripts/stream-host,
// embedded in this executable, never a package file) to %APPDATA%\sf4e\stream-host, with
// EmberFolder.txt naming the folder of this Launcher.exe, and returns that folder; empty when
// it could not be written. Scripts, README and EmberFolder.txt are replaced each time; the
// panel's own settings and log in that folder are kept.
std::wstring WriteStreamHostFiles();
} }
