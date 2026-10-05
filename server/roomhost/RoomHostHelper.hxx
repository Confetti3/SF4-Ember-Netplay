#pragma once

// The room host's own helper (sf4-net) process and the client that talks to
// it. This is the one platform seam of the program: Windows starts the helper
// on a named pipe (RoomHostHelperWindows.cxx); Unix runs it over stdio.
#include "RoomHost.hxx"
#include "../../src/platform/HelperClient.hxx"
#include <memory>
#include <string>

namespace sf4e { namespace roomhost {

class HelperLink {
public:
	virtual ~HelperLink() = default;
	virtual platform::HelperClient& Client() = 0;
	virtual bool Running() const = 0;
	// Waits up to graceMs for the helper to exit by itself, then ends it.
	virtual void Stop(unsigned graceMs) = 0;
};

// Starts the helper named by the configuration. Null with a reason in `error`.
std::unique_ptr<HelperLink> StartHelper(const Config& config, std::string& error);

} }
