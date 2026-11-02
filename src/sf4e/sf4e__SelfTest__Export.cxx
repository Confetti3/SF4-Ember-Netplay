#include "sf4e__SelfTest.hxx"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "../Dimps/Dimps__Game__Battle.hxx"
#include "../Dimps/Dimps__Game__Battle__System.hxx"
#include "../common/Localization.hxx"
#include "../common/ReplayRequest.hxx"
#include "../common/SkillBadge.hxx"
#include "../platform/Utf8.hxx"
#include "sf4e__ReplayStore.hxx"

// The export run: no test, but the same driving. From the main menu it makes
// each replay of the job file SF4E_SELFTEST_EXPORT names into a video, as
// Generate video on the Replays screen does, one after the other in this one
// game, and writes each video's path to the result file. The job file has a
// line a replay, its fields split by tabs:
//   path   name 1   name 2   line   wins 1   wins 2   class 1   class 2
// Empty names leave the names off and an empty line the line. Wins are the
// set score so far; "auto" for both counts it here, from each game's winner as
// the game decides it, from 0-0; anything else leaves the score off. A class
// is its band, 1 (D) to 10 (Grand Master), drawn on its name's plate; empty
// for none. Ember's mark is always on and the frame meter off
// (ember_render.py writes the file).
namespace sf4e { namespace selftest {
namespace {

namespace fs = std::filesystem;
using replaystore::Step;

enum class Stage { Next, Starting, Exporting, Done };

struct Job {
	std::string path;
	replay::Caption caption;
	bool counted = false;
};

struct Export : Steps<Stage> {
	std::vector<Job> jobs;
	std::size_t at = 0;
	std::uint64_t id = 0;
	int tries = 0;
	// The set so far, as the games played have ended, and the winner of the
	// one playing (-1 until its result is up); scored: every game ended with one.
	int wins[2] = {0, 0}, winner = -1;
	bool scored = true;
} s_export;

// The match's winner once the game shows its result, as the room reads it
// (sf4e__Game__Battle__System__NativeResult.cxx); -1 before then or for a draw.
int MatchWinner() {
	using System = Dimps::Game::Battle::System;
	using Dimps::Game::Battle::GameManager;
	const DWORD* flow = System::staticVars.CurrentBattleFlow;
	if (!flow || *flow != System::BF__MATCH_RESULT || !System::staticMethods.GetSingleton) return -1;
	System* const system = System::staticMethods.GetSingleton();
	GameManager* const manager = system && System::publicMethods.GetGameManager ? (system->*System::publicMethods.GetGameManager)() : nullptr;
	if (!manager || !GameManager::publicMethods.GetNativeResultIndex) return -1;
	const int index = (manager->*GameManager::publicMethods.GetNativeResultIndex)();
	return index == 0 || index == 1 ? index : -1;
}

std::vector<std::string> Fields(const std::string& line) {
	std::vector<std::string> fields;
	std::stringstream in(line);
	for (std::string field; std::getline(in, field, '\t');) fields.push_back(field);
	return fields;
}

bool Number(const std::string& text, int& value) {
	if (text.empty() || text.size() > 3 || text.find_first_not_of("0123456789") != std::string::npos) return false;
	value = std::stoi(text);
	return true;
}

// The job file's replays, or none with the reason said.
std::vector<Job> ReadJobs() {
	const std::wstring file = Environment(L"SF4E_SELFTEST_EXPORT");
	std::ifstream in{fs::path(file)};
	if (file.empty() || !in) { Say(false, "job file", "'" + platform::WideToUtf8(file) + "' cannot be read"); return {}; }
	std::vector<Job> jobs;
	for (std::string line; std::getline(in, line);) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty()) continue;
		auto fields = Fields(line);
		fields.resize(8);
		Job job;
		job.path = fields[0];
		replay::Caption& caption = job.caption;
		caption.names = !fields[1].empty() || !fields[2].empty();
		caption.name[0] = fields[1];
		caption.name[1] = fields[2];
		caption.line = !fields[3].empty();
		caption.text = fields[3];
		job.counted = fields[4] == "auto" && fields[5] == "auto";
		caption.set = job.counted || (Number(fields[4], caption.wins[0]) && Number(fields[5], caption.wins[1]));
		caption.mark = true;
		for (int side = 0; side < 2; ++side) {
			int band = 0;
			if (Number(fields[6 + side], band) && band >= 1 && band <= SkillBadge::Classes) caption.band[side] = static_cast<std::uint8_t>(band);
		}
		std::error_code missing;
		if (!fs::is_regular_file(platform::Utf8ToWide(job.path.c_str()), missing)) { Say(false, "job file", "no replay at '" + job.path + "'"); return {}; }
		jobs.push_back(std::move(job));
	}
	if (jobs.empty()) Say(false, "job file", "no replay in it");
	return jobs;
}

}

bool StartExport() {
	s_export.jobs = ReadJobs();
	if (s_export.jobs.empty()) return false;
	s_export.Enter(Stage::Next);
	return true;
}

void TickExport(bool atMainMenu) {
	Export& run = s_export;
	run.waited++;
	const replaystore::Status& status = replaystore::GetStatus();
	switch (run.stage) {
	case Stage::Next: {
		// Each export starts from the main menu, as the Replays screen sends it.
		if (!atMainMenu || status.step != Step::Idle || !replaystore::Ready()) {
			if (run.Late(120)) run.Fail("export " + std::to_string(run.at + 1), "the main menu never came back");
			break;
		}
		Job& job = run.jobs[run.at];
		if (job.counted) {
			job.caption.set = run.scored;
			job.caption.wins[0] = run.wins[0];
			job.caption.wins[1] = run.wins[1];
		}
		run.winner = -1;
		replay::Request request{replay::Mode::Export, job.path, job.caption};
		request.id = ++run.id;
		replaystore::Start(request, atMainMenu, true);
		spdlog::info("SelfTest: exporting {} of {}: {}", run.at + 1, run.jobs.size(), job.path);
		run.Enter(Stage::Starting);
		break;
	}
	case Stage::Starting:
		if (status.step != Step::Idle || status.exportEnded == run.id) { run.tries = 0; run.Enter(Stage::Exporting); break; }
		// Just after the main menu comes up the game is still reading its saves
		// and an import is refused: asked again each second, for half a minute,
		// as the replay test does.
		if (status.notice == loc::T("replays.not_ready") && run.tries < 30) {
			if (run.waited < kSecond) break;
			run.tries++;
			run.Enter(Stage::Next);
			break;
		}
		run.Fail("export " + std::to_string(run.at + 1), "not started: '" + status.notice + "'");
		break;
	case Stage::Exporting: {
		const int winner = MatchWinner();
		if (winner >= 0) run.winner = winner;
		if (status.exportEnded != run.id || status.step != Step::Idle) {
			if (run.Late(30 * 60)) run.Fail("export " + std::to_string(run.at + 1), "still running after half an hour");
			break;
		}
		const std::string& path = run.jobs[run.at].path;
		const std::string video = path.substr(0, path.rfind('.')) + ".mp4";
		if (run.winner >= 0) run.wins[run.winner]++;
		else run.scored = false;
		const std::string won = run.winner >= 0 ? " WINNER " + std::to_string(run.winner + 1) : " WINNER none";
		Say(status.exportSaved, "export " + std::to_string(run.at + 1), status.exportSaved ? "VIDEO " + video + won : "no video: '" + status.notice + "'");
		if (!status.exportSaved || ++run.at == run.jobs.size()) {
			run.Enter(Stage::Done);
			Finish();
		}
		else run.Enter(Stage::Next);
		break;
	}
	case Stage::Done:
		break;
	}
}

} }
