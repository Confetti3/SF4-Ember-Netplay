#pragma once
#include <string>
#include <spdlog/spdlog.h>

// The game builds its first device and Ember reads its settings inside the
// game's Main::Initialize, before Sidecar's logger exists (sf4e__Platform.cxx).
// Lines written from there are held here and written once it does.
namespace sf4e {
	namespace StartupLog {
		// Any thread. Written at once after Flush, held before it.
		void Log(spdlog::level::level_enum level, std::string message);
		// Once, as soon as the logger is up: writes what was held.
		void Flush();
	}
}
