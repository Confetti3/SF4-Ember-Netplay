#pragma once

#include <string>

// The encoder's end of the video export (VideoLink.hxx is the game's): the
// process the game starts takes the pictures out of the shared memory and
// hands them to VideoEncoder.
namespace sf4e { namespace platform { namespace videolink {

// Serves the link of that name until the game asks for the file to be closed
// or goes away. The process's exit code, a Result of VideoShared.hxx.
int Serve(const std::wstring& link);

} } }
