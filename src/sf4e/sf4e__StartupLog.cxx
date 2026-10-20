#include "sf4e__StartupLog.hxx"

#include <mutex>
#include <utility>
#include <vector>

namespace sf4e { namespace StartupLog {
namespace {
std::mutex mutex;
bool ready = false;
std::vector<std::pair<spdlog::level::level_enum, std::string>> pending;
}

void Log(spdlog::level::level_enum level, std::string message) {
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (!ready) { pending.emplace_back(level, std::move(message)); return; }
	}
	spdlog::log(level, message);
}

void Flush() {
	std::vector<std::pair<spdlog::level::level_enum, std::string>> held;
	{
		std::lock_guard<std::mutex> lock(mutex);
		ready = true;
		held.swap(pending);
	}
	for (const auto& line : held) spdlog::log(line.first, line.second);
}
} }
