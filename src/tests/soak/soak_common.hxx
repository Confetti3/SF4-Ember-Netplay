#pragma once

// What every part of the soak shares: the clock, the run's options, each room
// thread's random source and the events file. No Windows headers here, so this
// can come before the session headers.
#include "soak_verdict.hxx"
#include <atomic>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace test { namespace soak {

Clock Now(); // GetTickCount64
extern std::atomic<bool> stopRequested;
extern thread_local std::mt19937_64 rng; // one per thread: every room has its own thread
extern std::mutex outputMutex;
extern Clock runStart;
extern std::ofstream eventsFile;

inline std::uint64_t Uniform(std::uint64_t low, std::uint64_t high) { return std::uniform_int_distribution<std::uint64_t>(low, high)(rng); }
inline bool Chance(double probability) { return std::uniform_real_distribution<double>(0, 1)(rng) < probability; }
// seconds -> milliseconds in [low, high]
inline Clock Seconds(double low, double high) { return static_cast<Clock>(1000 * (low + (high - low) * std::uniform_real_distribution<double>(0, 1)(rng))); }

std::string Hms(Clock ms);
std::string UtcStamp();
// One line in the events file; failures also go to the console.
void Event(int room, int member, const std::string& what, bool console = true);

// Logs a call that held up the loop, since every member shares it and a stall
// there delays the chat round trips measured on it.
struct SlowCall {
	const char* what; int room, member; Clock began = Now();
	~SlowCall() { const auto took = Elapsed(Now(), began); if (took > 400) Event(room, member, std::string("slow call: ") + what + " took " + std::to_string(took) + " ms", false); }
};

struct Options {
	std::wstring helper;
	std::size_t members = 16;
	double minutes = 240;
	std::string log = "soak.csv";
	int firstRoom = 1;
	double matchEvery = 240, rejoinEvery = 180;
	// 1 is a quiet lobby (a chat line every 1.5 to 5 minutes and a move every 45 to 150 seconds per
	// member); larger values multiply the rate for stress.
	double activity = 1;
	// The run fails when a room goes past any of these (see Evaluate).
	Limits limits;
	std::vector<std::pair<int, std::string>> rooms; // room number, invitation
};
extern Options options;

} } }
