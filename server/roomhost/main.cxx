// sf4e-room-host: the authority of one public room, started by the ember-rooms
// supervisor (server/ember-rooms/README.md, "Child protocol").
//
// stdin:  one JSON configuration line, then end of file means "close the room".
// stdout: one JSON object per line (hosted, status, closed) and nothing else.
// stderr: the log.
#include "RoomHost.hxx"
#include "RoomHostHelper.hxx"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

using namespace sf4e;

namespace {
constexpr std::uint64_t TickMs = 16;
// The supervisor gives a room 30 s to say hosted, and 10 s to exit once its
// stdin closes; both limits here are inside those.
constexpr std::uint64_t HelperConnectMs = 15000, HostedMs = 25000, LeaveMs = 4000;
constexpr unsigned HelperExitMs = 2000;

std::uint64_t NowMs() {
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Emit(const std::string& line) {
	std::fwrite(line.data(), 1, line.size(), stdout);
	std::fputc('\n', stdout);
	std::fflush(stdout);
}

// The reader thread may still be blocked on stdin, which a normal exit would
// wait on while flushing streams; everything that matters is flushed here.
[[noreturn]] void Exit(int code) {
	spdlog::default_logger()->flush();
	std::fflush(stdout);
	std::fflush(stderr);
	std::_Exit(code);
}

// stdin, read on its own thread so the room keeps ticking: the first line is
// the configuration and the end of the stream is the order to close.
struct Input {
	std::mutex mutex;
	std::condition_variable changed;
	std::string config;
	bool haveConfig = false;
	std::atomic<bool> ended{false};

	void Start() {
		std::thread([this]() {
			std::string line;
			while (std::getline(std::cin, line)) {
				std::lock_guard<std::mutex> lock(mutex);
				if (haveConfig) continue; // Nothing else is sent; later lines are ignored.
				config = line; haveConfig = true;
				changed.notify_all();
			}
			ended = true;
			std::lock_guard<std::mutex> lock(mutex);
			changed.notify_all();
		}).detach();
	}
	bool WaitForConfig(std::string& line) {
		std::unique_lock<std::mutex> lock(mutex);
		changed.wait(lock, [this]() { return haveConfig || ended.load(); });
		if (!haveConfig) return false;
		line = config;
		return true;
	}
};
}

int main() {
	// Every library line goes to stderr: stdout carries only the protocol.
	spdlog::set_default_logger(spdlog::stderr_logger_mt("room-host"));
	spdlog::flush_on(spdlog::level::info);

	static Input input; // Outlives main for the detached reader.
	input.Start();
	std::string line;
	if (!input.WaitForConfig(line)) { spdlog::error("No configuration line on stdin"); Exit(2); }
	roomhost::Config config;
	std::string error;
	if (!roomhost::ParseConfig(line, config, error)) {
		spdlog::error("Invalid configuration field: {}", error);
		Emit(roomhost::ClosedLine("invalid_config"));
		Exit(2);
	}
	auto helper = roomhost::StartHelper(config, error);
	if (!helper) {
		spdlog::error("Helper could not be started: {}", error);
		Emit(roomhost::ClosedLine("helper_unavailable"));
		Exit(1);
	}
	const auto fail = [&](const std::string& reason) {
		spdlog::error("Room host closing: {}", reason);
		Emit(roomhost::ClosedLine(reason));
		helper->Client().Send("{\"type\":\"shutdown\"}");
		helper->Stop(HelperExitMs);
		Exit(1);
	};
	const auto started = NowMs();
	while (helper->Client().State() == platform::HelperState::Connecting && !input.ended) {
		if (NowMs() - started >= HelperConnectMs) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	if (!input.ended && helper->Client().State() != platform::HelperState::Connected) fail("helper_unavailable");

	const auto room = std::make_shared<session::IrohRoom>(helper->Client());
	roomhost::RoomHost host(room, config, Emit);
	if (!input.ended && !host.Open()) fail("host_refused");

	while (!input.ended && !host.Closing()) {
		const auto tick = NowMs();
		if (!host.Tick(tick)) fail(host.Error());
		if (!host.Hosted() && tick - started >= HostedMs) fail("host_timeout");
		const auto spent = NowMs() - tick;
		if (spent < TickMs) std::this_thread::sleep_for(std::chrono::milliseconds(TickMs - spent));
	}

	// The supervisor closed stdin, or the room ended on its own: finish the
	// close, stop the helper, exit 0. A room that ended on its own says closed
	// first, so the supervisor stops listing it now rather than at exit; that
	// closes stdin and starts the same 10 s allowance as above.
	if (host.Closing() && !input.ended) {
		spdlog::info("Room host: room ended ({}); leaving", host.CloseReason());
		Emit(roomhost::ClosedLine(host.CloseReason()));
	} else spdlog::info("Room host: stdin closed; closing the room");
	host.BeginClose(NowMs());
	while (!host.CloseDelivered(NowMs()) && host.Tick(NowMs())) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	host.Leave();
	const auto leaving = NowMs();
	while (host.Leaving() && NowMs() - leaving < LeaveMs) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	helper->Client().Send("{\"type\":\"shutdown\"}");
	helper->Stop(HelperExitMs);
	Exit(0);
}
