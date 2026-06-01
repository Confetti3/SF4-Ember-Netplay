#include "electron_ipc.hxx"

#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "netplay/netplay_launch_controller.hxx"

namespace sf4e {
namespace launcher {

	namespace {

		bool ReadLine(std::string& outLine) {
			outLine.clear();
			if (!std::getline(std::cin, outLine)) {
				return false;
			}
			while (!outLine.empty() && (outLine.back() == '\r' || outLine.back() == '\n')) {
				outLine.pop_back();
			}
			return true;
		}

		void WriteJsonLine(const nlohmann::json& j) {
			std::cout << j.dump() << std::endl;
			std::cout.flush();
		}

	} // namespace

	bool RunElectronIpcBridge(NetplayLaunchController& controller) {
		WriteJsonLine(controller.BuildStateJson());

		std::string line;
		while (ReadLine(line)) {
			if (line.empty()) {
				continue;
			}

			try {
				nlohmann::json msg = nlohmann::json::parse(line);
				nlohmann::json reply = controller.HandleWebMessage(msg);
				if (!reply.is_null() && !reply.empty()) {
					WriteJsonLine(reply);
				}

				if (controller.ShouldExitForUpdate()) {
					nlohmann::json evt;
					evt["type"] = "exitForUpdate";
					evt["v"] = 1;
					WriteJsonLine(evt);
					return false;
				}

				if (controller.IsFinished()) {
					nlohmann::json evt;
					evt["type"] = "launcherFinished";
					evt["v"] = 1;
					evt["cancelled"] = controller.WasCancelled();
					WriteJsonLine(evt);
					return !controller.WasCancelled();
				}
			}
			catch (const std::exception& e) {
				nlohmann::json err;
				err["v"] = 1;
				err["type"] = "error";
				err["message"] = std::string("Invalid IPC message: ") + e.what();
				WriteJsonLine(err);
			}
			catch (...) {
				nlohmann::json err;
				err["v"] = 1;
				err["type"] = "error";
				err["message"] = "Invalid IPC message.";
				WriteJsonLine(err);
			}
		}

		return false;
	}

} // namespace launcher
} // namespace sf4e
