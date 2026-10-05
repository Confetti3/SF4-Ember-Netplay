// Soak harness for public rooms on a room host elsewhere (a Linux
// sf4e-room-host started by hand, server/roomhost/soak/README.md). Not a test:
// it needs hosts to talk to, so it is built but not registered with ctest.
//
//   PublicRoomSoakTest <sf4-net.exe> <room_ticket.exe> [--members 16] [--minutes 240]
//       [--log soak.csv] [--seed N] [--first-room N] [--match-every SECONDS]
//       [--rejoin-every SECONDS] [--activity X] [--max-<limit> N ...] <invitation> [<invitation> ...]
//   PublicRoomSoakTest --print-config <room_ticket.exe>
//
// Each invitation is one room. Room N (1 for the first invitation, or from
// --first-room; an invitation can also be written N=<invitation>) has the room
// id server/roomhost/soak/soak-hosts.sh gives host N, so the tickets minted here
// are the ones that host admits. Every room is filled with --members simulated
// members, each with its own helper process and Ember ID (the first member is
// the creator the hosts are configured with), and each keeps doing what a
// player does, rate-limited and spread over time: chat, seat changes, fighter
// changes, Ready and Unready, connection checks, a match now and then between
// the two fighters at table 0, and a random member leaving and rejoining every
// few minutes. No GGPO session is started (as in PublicRoomHostTest).
//
// Failures are counted, not fatal. One CSV row per room per minute (counters
// are running totals, latencies are for that minute), an events file beside it
// (<log>.events.log) and a summary at the end. Each room runs on its own thread,
// because every member of a room imports every commit and a real room spreads
// that over 16 PCs. The exit code is non-zero when a room breaks one of the
// limits in Evaluate: it was lost, most members could not stay connected, too
// many actions timed out, chat lagged or lines went missing, matches failed,
// members were dropped, or the client itself was saturated.
//
// This file holds the arguments and the main loop. The rest is in soak/:
// soak_member.cxx and soak_room.cxx (the workload), soak_metrics.cxx (the CSV),
// soak_verdict.cxx (the criteria) and soak_summary.cxx (the summary).
#include "soak/soak_workload.hxx"
#include "soak/soak_metrics.hxx"
#include "soak/soak_summary.hxx"
#include "public_room_support.hxx"
#include <iostream>
#include <thread>

using namespace sf4e;
using namespace sf4e::test::publicroom;
using namespace sf4e::test::soak;

namespace {
BOOL WINAPI OnConsole(DWORD) { stopRequested = true; return TRUE; }

// Prints what the hosts must be configured with (server/roomhost/soak/soak-hosts.sh
// carries the same values) so the two sides can be compared.
int PrintConfig(const std::wstring& tool) {
	ticketTool = tool;
	const auto key = Tool("key " + Seed('1'));
	CHECK(key.size() == 2);
	std::cout << "bridge_id=" << Bridge << "\nbuild_id=" << Build << "\nroom_id_prefix=" << std::string(RoomIdHex).substr(0, 30)
		<< " (room N appends its number as two hex digits)\nticket_key=" << key[0] << "\nticket_kid=" << key[1]
		<< "\ncreator=" << Tool("ember-id " + Seed('a')).at(0) << std::endl;
	return 0;
}

std::string RoomIdFor(int number) {
	char suffix[4];
	std::snprintf(suffix, sizeof(suffix), "%02x", number & 0xff);
	return std::string(RoomIdHex).substr(0, 30) + suffix;
}
}

int wmain(int argc, wchar_t** argv) {
	std::cout << std::unitbuf;
	if (argc == 3 && std::wstring(argv[1]) == L"--print-config") return PrintConfig(argv[2]);
	std::uint64_t seed = std::random_device()();
	std::vector<std::wstring> positional;
	for (int i = 1; i < argc; ++i) {
		const std::wstring arg = argv[i];
		const auto value = [&]() { CHECK(i + 1 < argc); return std::wstring(argv[++i]); };
		if (arg == L"--members") options.members = std::stoul(value());
		else if (arg == L"--minutes") options.minutes = std::stod(value());
		else if (arg == L"--log") options.log = Utf8(value());
		else if (arg == L"--seed") seed = std::stoull(value());
		else if (arg == L"--first-room") options.firstRoom = std::stoi(value());
		else if (arg == L"--match-every") options.matchEvery = std::stod(value());
		else if (arg == L"--rejoin-every") options.rejoinEvery = std::stod(value());
		else if (arg == L"--activity") options.activity = std::stod(value());
		else if (arg == L"--max-timeout-pct") options.limits.maxTimeoutPct = std::stod(value());
		else if (arg == L"--max-chat-p95-ms") options.limits.maxChatP95Ms = std::stod(value());
		else if (arg == L"--max-chat-lost-pct") options.limits.maxChatLostPct = std::stod(value());
		else if (arg == L"--max-chat-missing-pct") options.limits.maxChatMissingPct = std::stod(value());
		else if (arg == L"--max-match-fail-pct") options.limits.maxMatchFailPct = std::stod(value());
		else if (arg == L"--max-drops-per-member-hour") options.limits.maxDropsPerMemberHour = std::stod(value());
		else if (arg == L"--max-join-fail-pct") options.limits.maxJoinFailPct = std::stod(value());
		else positional.push_back(arg);
	}
	if (positional.size() < 3 || options.members < 2 || options.members > room::MaximumMembers) {
		std::cerr << "usage: PublicRoomSoakTest <sf4-net.exe> <room_ticket.exe> [--members 2..16] [--minutes M] [--log csv] [--seed N] "
			"[--first-room N] [--match-every S] [--rejoin-every S] <invitation> [<invitation> ...]\n"
			"       PublicRoomSoakTest --print-config <room_ticket.exe>" << std::endl;
		return 2;
	}
	rng.seed(seed);
	options.helper = positional[0];
	ticketTool = positional[1];
	int nextNumber = options.firstRoom;
	for (std::size_t i = 2; i < positional.size(); ++i) {
		std::string text = Utf8(positional[i]);
		int number = nextNumber;
		const auto equals = text.find('=');
		if (equals != std::string::npos && equals < 4) { number = std::stoi(text.substr(0, equals)); text = text.substr(equals + 1); }
		CHECK(number >= 1 && number <= 255);
		options.rooms.emplace_back(number, text);
		nextNumber = number + 1;
	}
	std::vector<std::string> key;
	CHECK(RunTool("key " + Seed('1'), key) && key.size() == 2);
	const std::string kid = key[1];

	SetConsoleCtrlHandler(OnConsole, TRUE);
	runStart = Now();
	eventsFile.open(options.log + ".events.log", std::ios::app);
	std::ofstream csv(options.log, std::ios::app);
	CHECK(csv.is_open());
	if (csv.tellp() == 0) csv << CsvHeader << std::endl;
	std::cout << "seed " << seed << ": " << options.rooms.size() << " room(s) of " << options.members << " members for " << options.minutes
		<< " minutes; csv " << options.log << std::endl;
	Event(0, -1, "soak start, seed " + std::to_string(seed), false);

	// The people. Member 0 of every room is the creator the hosts are configured with.
	std::vector<std::unique_ptr<Room>> rooms;
	for (const auto& entry : options.rooms) {
		auto room = std::make_unique<Room>();
		room->number = entry.first;
		room->invitation = entry.second;
		room->roomIdHex = RoomIdFor(entry.first);
		room->kid = kid;
		room->started = Now();
		for (std::size_t index = 0; index < options.members; ++index) {
			auto member = std::make_unique<Member>();
			char hex[8];
			std::snprintf(hex, sizeof(hex), "%02x%02x", entry.first & 0xff, static_cast<int>(index));
			member->room = room.get();
			member->index = static_cast<int>(index);
			char label[16];
			std::snprintf(label, sizeof(label), "r%02dm%02d", entry.first, static_cast<int>(index));
			member->label = label;
			member->seed = index == 0 ? Seed('a') : std::string(60, '7') + hex;
			std::vector<std::string> ember;
			CHECK(RunTool("ember-id " + member->seed, ember) && !ember.empty());
			member->emberId = ember[0];
			// The creator first, the rest a second and a half apart.
			member->nextAt = Now() + (index == 0 ? 0 : Seconds(3, 4) + index * 1500) + (rooms.size() * 1000);
			room->members.push_back(std::move(member));
		}
		room->nextMatchAt = Now() + Seconds(90, 120);
		room->nextRejoinAt = Now() + Seconds(120, 150);
		rooms.push_back(std::move(room));
	}
	const Clock endAt = runStart + static_cast<Clock>(options.minutes * 60000);
	for (auto& room : rooms) allRooms.push_back(room.get());
	std::vector<std::thread> threads;
	for (auto& room : rooms) threads.emplace_back(RoomMain, room.get(), endAt, &csv, seed);

	// The main thread only reports: the process's own processor use for the rows,
	// and how many members are connected.
	Clock nextStatus = runStart + 60000, nextCpu = runStart + 58000, lastCpuAt = runStart;
	std::uint64_t lastSelf = 0;
	while (!stopRequested && Now() < endAt) {
		Sleep(200);
		const auto now = Now();
		if (now >= nextCpu) {
			nextCpu += 60000;
			const auto self = ProcessTime(GetCurrentProcess());
			processCpuPct = 100.0 * (self - lastSelf) / (std::max<Clock>)(1, Elapsed(now, lastCpuAt));
			lastSelf = self; lastCpuAt = now;
		}
		if (now >= nextStatus) {
			nextStatus += 60000;
			std::size_t active = 0, total = 0;
			for (auto* room : allRooms) { active += room->activeNow; total += room->members.size(); }
			std::lock_guard<std::mutex> lock(outputMutex);
			std::cout << "[+" << Hms(Elapsed(now, runStart)) << "] " << active << "/" << total << " members connected" << std::endl;
		}
	}
	// Leave cleanly: every room thread ends its members and helpers.
	std::cout << (stopRequested ? "interrupted; " : "time is up; ") << "stopping helpers" << std::endl;
	stopRequested = true;
	for (auto& thread : threads) thread.join();
	bool failed = false;
	PrintSummary(rooms, Elapsed(Now(), runStart), failed);
	Event(0, -1, std::string("soak end: ") + (failed ? "FAILED" : "PASSED"), false);
	return failed ? 1 : 0;
}
