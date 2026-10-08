#include "sf4e__ReplayCapture.hxx"

#include <atomic>
#include <mutex>
#include <windows.h>
#include <d3d9.h>
#include <spdlog/spdlog.h>

#include "../platform/FrameGrab.hxx"
#include "../platform/VideoLink.hxx"

namespace {
namespace grab = sf4e::platform::grab;
namespace link = sf4e::platform::videolink;
using sf4e::replaycapture::State;

// What the game thread asks: under s_lock with the file, since Frame reads both.
std::atomic<State> s_state{State::Idle};
std::atomic<bool> s_wanted{false}, s_sending{false}, s_afterOverlay{false};
// The grab and the link, and everything below: the render thread's, under
// s_lock so a reset on another thread waits for a frame in progress.
std::mutex s_lock;
std::wstring s_file;
bool s_opened = false;
bool s_deviceAvailable = true;
ULONGLONG s_closingSince = 0;
// The encoder's process may take this long to close its file before it is ended.
constexpr ULONGLONG kClosePatienceMs = 30000;
}

namespace sf4e { namespace replaycapture {

void Begin(const std::wstring& file, bool withOverlay) {
	std::lock_guard<std::mutex> lock(s_lock);
	if (s_state != State::Idle) return;
	s_file = file; s_opened = false; s_sending = false; s_afterOverlay = withOverlay;
	s_wanted = true;
	s_state = State::Recording;
}

void End() { s_wanted = false; }

void Clear() {
	const State state = s_state;
	if (state == State::Done || state == State::Failed) s_state = State::Idle;
}

State GetState() { return s_state; }

bool AfterOverlay() { return s_afterOverlay && s_state == State::Recording; }

void Frame(IDirect3DDevice9* device) {
	const State seen = s_state;
	if (seen != State::Recording && seen != State::Closing) return;
	std::lock_guard<std::mutex> lock(s_lock);
	if (s_state == State::Recording && !s_wanted) {
		// Asked to stop: the encoder closes the file on its own time.
		grab::Release();
		if (s_sending) { link::Stop(); s_closingSince = GetTickCount64(); s_state = State::Closing; }
		else s_state = State::Failed;
		s_sending = false;
	}
	else if (s_state == State::Recording) {
		if (!s_deviceAvailable) return;
		if (!s_opened) {
			// The first frame tells the picture's size.
			s_opened = true;
			unsigned width = 0, height = 0;
			s_sending = grab::Open(device, width, height) && link::Start(s_file, width, height);
			if (!s_sending) { grab::Release(); s_wanted = false; s_state = State::Failed; return; }
		}
		if (!grab::Grab(device, link::Send)) {
			link::Fail();
			grab::Release(); s_wanted = false; s_sending = false;
			s_closingSince = GetTickCount64(); s_state = State::Closing;
		}
	}
	if (s_state == State::Closing) {
		bool ok = false;
		if (link::Closed(ok)) s_state = ok ? State::Done : State::Failed;
		else if (GetTickCount64() - s_closingSince > kClosePatienceMs) { link::Abort(); s_state = State::Failed; }
	}
}

void Release() {
	std::lock_guard<std::mutex> lock(s_lock);
	s_deviceAvailable = false;
	grab::Release();
}

void Resume(IDirect3DDevice9* device) {
	std::lock_guard<std::mutex> lock(s_lock);
	s_deviceAvailable = device && device->TestCooperativeLevel() == D3D_OK;
}

} }
