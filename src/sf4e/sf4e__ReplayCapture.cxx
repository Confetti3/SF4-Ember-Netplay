#include "sf4e__ReplayCapture.hxx"

#include <atomic>
#include <mutex>
#include <windows.h>
#include <spdlog/spdlog.h>

#include "../platform/FrameGrab.hxx"
#include "../platform/VideoLink.hxx"

namespace {
namespace grab = sf4e::platform::grab;
namespace link = sf4e::platform::videolink;
using sf4e::replaycapture::Failure;
using sf4e::replaycapture::State;

// Written under s_lock; read without it where a stale answer only costs a frame.
std::atomic<State> s_state{State::Idle};
// The grab holds objects of the device: Frame has them to let go of.
// afterOverlay: this export's frames are taken once Ember's overlay is drawn.
std::atomic<bool> s_grabbing{false}, s_afterOverlay{false};
// The grab, the link and everything below.
std::mutex s_lock;
std::wstring s_file;
Failure s_failure = Failure::None;
// wanted: the game thread has not asked to stop. linked: the encoder's
// process was started. resetting: the device is between Release and Restored.
bool s_wanted = false, s_linked = false, s_resetting = false;
ULONGLONG s_closingSince = 0;
// The encoder's process may take this long to close its file before it is ended.
constexpr ULONGLONG kClosePatienceMs = 30000;

Failure FailureOf(link::Result result) {
	return result == link::Written ? Failure::None : result == link::NotWritable || result == link::NotNamed ? Failure::File : Failure::Encoder;
}
}

namespace sf4e { namespace replaycapture {

void Begin(const std::wstring& file, bool withOverlay) {
	std::lock_guard<std::mutex> lock(s_lock);
	if (s_state != State::Idle) return;
	s_file = file; s_failure = Failure::None; s_afterOverlay = withOverlay;
	s_wanted = true; s_linked = false;
	s_state = State::Recording;
}

void End() {
	std::lock_guard<std::mutex> lock(s_lock);
	s_wanted = false;
}

State Poll() {
	link::Abandoned stuck;
	{
		std::lock_guard<std::mutex> lock(s_lock);
		if (s_state == State::Recording) {
			if (s_failure == Failure::None && s_linked && link::Lost()) s_failure = Failure::Encoder;
			if (s_failure != Failure::None || !s_wanted) {
				if (s_linked) {
					// The encoder closes the file on its own time; one that lost pictures is not kept.
					link::Stop(s_failure == Failure::None);
					s_closingSince = GetTickCount64();
					s_state = State::Closing;
				}
				else {
					// Stopped before a frame was drawn: there was nothing to record.
					if (s_failure == Failure::None) s_failure = Failure::Picture;
					s_state = State::Failed;
				}
			}
		}
		if (s_state == State::Closing) {
			link::Result result = link::NoLink;
			if (link::Closed(result)) {
				if (s_failure == Failure::None) s_failure = FailureOf(result);
				s_state = s_failure == Failure::None ? State::Done : State::Failed;
			}
			else if (GetTickCount64() - s_closingSince > kClosePatienceMs) {
				stuck = link::Abandon();
				s_failure = Failure::Encoder;
				s_state = State::Failed;
			}
		}
	}
	link::End(stuck);
	return s_state;
}

State GetState() { return s_state; }

bool AfterOverlay() { return s_afterOverlay && s_state == State::Recording; }

Failure Why() {
	std::lock_guard<std::mutex> lock(s_lock);
	return s_failure;
}

void Clear() {
	std::lock_guard<std::mutex> lock(s_lock);
	if (s_state == State::Done || s_state == State::Failed) s_state = State::Idle;
}

void Frame(IDirect3DDevice9* device) {
	if (s_state != State::Recording && !s_grabbing) return;
	std::lock_guard<std::mutex> lock(s_lock);
	if (s_resetting) return;
	if (s_state != State::Recording || !s_wanted || s_failure != Failure::None) {
		grab::Release(); s_grabbing = false;
		return;
	}
	if (!s_linked) {
		// The first frame tells the picture's size.
		unsigned width = 0, height = 0;
		if (!grab::Open(device, width, height)) { s_failure = Failure::Picture; return; }
		s_grabbing = true;
		if (!link::Start(s_file, width, height)) { s_failure = Failure::Encoder; return; }
		s_linked = true;
	}
	// A picture that changed size, or that the device would not hand over,
	// ends the export: a file with part of the replay is not what was asked for.
	if (!grab::Grab(device, link::Send)) {
		spdlog::warn("Replay capture: the game's picture could not be read; the export ends without a file");
		s_failure = Failure::Picture;
	}
}

void Release() {
	std::lock_guard<std::mutex> lock(s_lock);
	grab::Release();
	s_resetting = true;
}

void Restored() {
	std::lock_guard<std::mutex> lock(s_lock);
	s_resetting = false;
}

} }
