// Windows: the helper is started the way the launcher starts it for a game,
// on a named pipe, in a job that ends it with this process.
#include "RoomHostHelper.hxx"
#include "../../src/platform/HelperProcess.hxx"

namespace sf4e { namespace roomhost {
namespace {
class WindowsHelperLink final : public HelperLink {
public:
	platform::HelperProcess process;
	platform::HelperClient client;
	platform::HelperClient& Client() override { return client; }
	bool Running() const override { return process.IsRunning(); }
	// The process first: the client still has the shutdown command to deliver.
	void Stop(unsigned graceMs) override { process.Stop(graceMs); client.Stop(); }
};

std::wstring Widen(const std::string& text) {
	if (text.empty()) return {};
	const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (length <= 0) return {};
	std::wstring wide(static_cast<std::size_t>(length), L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), &wide[0], length);
	return wide;
}
}

std::unique_ptr<HelperLink> StartHelper(const Config& config, std::string& error) {
	// config.port and config.coordinationPort are the Unix helper's
	// --bind-port and --coordination-port. The Windows helper takes no ports
	// on its command line and chooses its own, so they are not used here.
	const auto path = Widen(config.helper);
	if (path.empty()) { error = "helper_path"; return {}; }
	std::unique_ptr<WindowsHelperLink> link(new WindowsHelperLink());
	if (!link->process.Start(path, GetCurrentProcessId())) {
		error = "helper_start_" + std::to_string(link->process.LastError());
		return {};
	}
	if (!link->client.Start(link->process.Bootstrap())) { error = "helper_pipe"; return {}; }
	return std::unique_ptr<HelperLink>(link.release());
}

} }
