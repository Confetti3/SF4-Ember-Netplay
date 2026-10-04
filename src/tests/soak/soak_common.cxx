#include "soak_common.hxx"
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>

namespace sf4e { namespace test { namespace soak {

Clock Now() { return GetTickCount64(); }
std::atomic<bool> stopRequested{false};
thread_local std::mt19937_64 rng;
std::mutex outputMutex;
Clock runStart = 0;
std::ofstream eventsFile;
Options options;

std::string Hms(Clock ms) {
	char text[32];
	std::snprintf(text, sizeof(text), "%02llu:%02llu:%02llu", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60);
	return text;
}
std::string UtcStamp() {
	const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::tm parts;
	gmtime_s(&parts, &now);
	char text[40];
	std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &parts);
	return text;
}

void Event(int room, int member, const std::string& what, bool console) {
	char tag[32];
	if (member >= 0) std::snprintf(tag, sizeof(tag), "r%02d m%02d", room, member);
	else std::snprintf(tag, sizeof(tag), "r%02d    ", room);
	const std::string line = "[+" + Hms(Now() - runStart) + "] " + tag + " " + what;
	std::lock_guard<std::mutex> lock(outputMutex);
	if (eventsFile.is_open()) eventsFile << UtcStamp() << ' ' << line << std::endl;
	if (console) std::cout << line << std::endl;
}

} } }
