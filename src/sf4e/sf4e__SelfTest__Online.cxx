#include "sf4e__SelfTest__Online.hxx"

#include <fstream>

#include <windows.h>

#include "../Dimps/Dimps__Game__Battle.hxx"

namespace sf4e { namespace selftest { namespace online {

using NetplayFacade::RuntimeCommand;
using NetplayFacade::RuntimeSnapshot;

std::shared_ptr<const RuntimeSnapshot> Now() { return NetplayFacade::GetRuntimeSnapshotShared(); }

RuntimeCommand Command(netplay::CommandKind kind) {
	const auto now = Now();
	RuntimeCommand request;
	request.command = { kind, now->session.generation, {} };
	request.preferences = now->preferences;
	request.displayName = now->preferences.displayName;
	return request;
}

netplay::PlayerPreferences TestPreferences(const RuntimeSnapshot& now, bool host) {
	netplay::PlayerPreferences preferences = now.preferences;
	preferences.displayName = host ? "SelfTest host" : "SelfTest guest";
	preferences.autoInputDelay = false;
	preferences.inputDelay = 2;
	preferences.tableRules.roundCount = 1;
	return preferences;
}

void UseKeyboard(const RuntimeSnapshot& now) {
	RuntimeCommand request;
	request.command.generation = now.session.generation;
	request.inputAction = input::Action::UseKeyboard;
	NetplayFacade::SubmitRuntimeCommand(std::move(request));
}

void ReadyAsRyu(const RuntimeSnapshot& now, int table) {
	const auto& seats = now.Room().tables[table];
	if (!seats.p1 || !seats.p2 || now.session.match != netplay::MatchState::None || now.session.readyPending) return;
	RuntimeCommand request = Command(netplay::CommandKind::Ready);
	request.character.charaID = 0; request.character.unc_edition = Dimps::Game::Battle::ED_USF4;
	NetplayFacade::SubmitRuntimeCommand(std::move(request));
}

std::string ReadLine(const std::wstring& path) {
	std::ifstream in(path);
	std::string line;
	std::getline(in, line);
	while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
	return line;
}

bool WriteLine(const std::wstring& path, const std::string& line) {
	const std::wstring temp = path + L".tmp";
	{
		std::ofstream out(temp, std::ios::trunc);
		out << line << '\n';
		out.flush();
		if (!out) return false;
	}
	return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

} } }
