// Unix: the helper is a child running `sf4-net --stdio`, its stdin and stdout
// piped to the client and its stderr inherited (the supervisor collects it).
// Closing its stdin is its order to stop, so it cannot outlive this process:
// when the room host dies its end of the pipe closes with it.
#include "RoomHostHelper.hxx"
#include <spdlog/spdlog.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace sf4e { namespace roomhost {
namespace {
class PosixHelperLink final : public HelperLink {
public:
	platform::HelperClient client;
	mutable pid_t pid = 0; // 0 once reaped; Running() is a const query that also reaps.
	~PosixHelperLink() override { Stop(0); }
	platform::HelperClient& Client() override { return client; }
	bool Running() const override {
		if (!pid) return false;
		int status = 0;
		const pid_t ended = waitpid(pid, &status, WNOHANG);
		if (ended == pid) { exited = true; exitStatus = status; pid = 0; }
		return pid != 0;
	}
	// The client first: closing the helper's stdin tells it to stop; then wait
	// for its exit up to graceMs, and end it if it is still there.
	void Stop(unsigned graceMs) override {
		client.Stop();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(graceMs);
		while (Running() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
		if (!pid) { Report(); return; }
		spdlog::warn("Helper still running {} ms after its stdin closed; killing it", graceMs);
		kill(pid, SIGKILL);
		int status = 0;
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
		pid = 0; exited = true; exitStatus = status;
		Report();
	}

private:
	void Report() {
		if (!exited || reported) return;
		reported = true;
		if (WIFEXITED(exitStatus)) spdlog::info("Helper exited with status {}", WEXITSTATUS(exitStatus));
		else if (WIFSIGNALED(exitStatus)) spdlog::info("Helper ended by signal {}", WTERMSIG(exitStatus));
	}
	mutable int exitStatus = 0;
	mutable bool exited = false;
	bool reported = false;
};

struct Pipe {
	int fds[2] = {-1, -1};
	~Pipe() { Close(0); Close(1); }
	bool Open() { return pipe2(fds, O_CLOEXEC) == 0; }
	void Close(int end) { if (fds[end] >= 0) { close(fds[end]); fds[end] = -1; } }
	int Take(int end) { const int fd = fds[end]; fds[end] = -1; return fd; }
};
}

std::unique_ptr<HelperLink> StartHelper(const Config& config, std::string& error) {
	if (config.helper.empty()) { error = "helper_path"; return {}; }
	std::vector<std::string> arguments{config.helper, "--stdio"};
	// A port of 0 (absent on a hand-written line) leaves the helper its default.
	if (config.port) { arguments.push_back("--bind-port"); arguments.push_back(std::to_string(config.port)); }
	if (config.coordinationPort) { arguments.push_back("--coordination-port"); arguments.push_back(std::to_string(config.coordinationPort)); }
	std::vector<char*> argv;
	for (auto& argument : arguments) argv.push_back(&argument[0]);
	argv.push_back(nullptr);

	Pipe toHelper, fromHelper; // [0] read end, [1] write end
	if (!toHelper.Open() || !fromHelper.Open()) { error = "helper_pipe"; return {}; }
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	// dup2 clears the close-on-exec flag on the child's copies; every other fd
	// of this process is close-on-exec or a standard stream.
	posix_spawn_file_actions_adddup2(&actions, toHelper.fds[0], STDIN_FILENO);
	posix_spawn_file_actions_adddup2(&actions, fromHelper.fds[1], STDOUT_FILENO);
	std::unique_ptr<PosixHelperLink> link(new PosixHelperLink());
	const int spawned = posix_spawn(&link->pid, config.helper.c_str(), &actions, nullptr, argv.data(), environ);
	posix_spawn_file_actions_destroy(&actions);
	if (spawned != 0) { link->pid = 0; error = "helper_start_" + std::to_string(spawned); return {}; }
	toHelper.Close(0); fromHelper.Close(1);
	// The pipes keep their ends until the client has them, so a failed start
	// closes them (and the link's destructor ends the helper).
	if (!link->client.Start(fromHelper.fds[0], toHelper.fds[1])) { error = "helper_pipe"; return {}; }
	fromHelper.Take(0); toHelper.Take(1);
	spdlog::info("Helper started pid={} port={} coordination_port={}", link->pid, config.port, config.coordinationPort);
	return std::unique_ptr<HelperLink>(link.release());
}

} }
