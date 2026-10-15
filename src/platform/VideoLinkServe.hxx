#pragma once

#include <string>

// The encoder's end of the video link (VideoLink.hxx), in Launcher.exe
// started with --encode-video.
namespace sf4e { namespace platform { namespace videolink {

// Serves the link of that name until the game stops it or ends. The
// process's exit code, 0 for a file that holds a video under its name.
int Serve(const std::wstring& link);

} } }
