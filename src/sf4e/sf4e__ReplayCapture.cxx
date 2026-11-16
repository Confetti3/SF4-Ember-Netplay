#include "sf4e__ReplayCapture.hxx"

#include <atomic>
#include <mutex>
#include <windows.h>
#include <d3d9.h>
#include <spdlog/spdlog.h>

#include "../platform/FrameGrab.hxx"
#include "../platform/VideoLink.hxx"
#include "../common/EnvFlag.hxx"

namespace {
namespace grab = sf4e::platform::grab;
namespace link = sf4e::platform::videolink;
using sf4e::replaycapture::State;

// What the game thread asks: under s_lock with the file, since Frame reads both.
std::atomic<State> s_state{State::Idle};
std::atomic<bool> s_wanted{false};
// The grab, the link and everything below: under s_lock, so a reset on
// another thread waits for a frame in progress, and a cancel on the game
// thread reaches the link that Frame opened.
std::mutex s_lock;
std::wstring s_file, s_encoder;
bool s_opened = false, s_sending = false;
bool s_deviceAvailable = true;
// A fast export (sf4e__ReplayCapture.hxx): asked at Begin, and running once
// the sound is held and the encoder took it. The pictures sent so far and the
// sound made for the next one: the thread that renders, under s_lock.
sf4e::replaycapture::SoundSource s_source{};
bool s_fastAsked = false;
std::atomic<bool> s_fast{false};
unsigned long long s_pictures = 0;
std::vector<float> s_sound;

// The game's sound goes back to the speakers once a fast export stops sending.
void EndFast() {
	if (s_fast.exchange(false) && s_source.release) s_source.release();
}
}

namespace sf4e { namespace replaycapture {

void Begin(const std::wstring& file, const std::wstring& encoder) {
	std::lock_guard<std::mutex> lock(s_lock);
	if (s_state != State::Idle) return;
	s_file = file; s_encoder = encoder; s_opened = false; s_sending = false;
	s_fastAsked = sf4e::EnvFlag("SF4E_EXPORT_FAST");
	s_wanted = true;
	s_state = State::Recording;
}

void End() { s_wanted = false; }

bool Cancel() {
	std::lock_guard<std::mutex> lock(s_lock);
	if (s_state != State::Recording) return false;
	s_wanted = false;
	// The encoder discards its file from here, whether or not another frame
	// is drawn: on its own when the game goes (VideoLinkServe.cxx). The grab's
	// surfaces are the render thread's, which lets go of them on its next
	// Frame. With nothing open yet, nothing will be.
	if (s_sending) link::Fail();
	else s_state = State::Failed;
	EndFast();
	return true;
}

void Clear() {
	const State state = s_state;
	if (state == State::Done || state == State::Failed) s_state = State::Idle;
}

State GetState() { return s_state; }

void SetSoundSource(const SoundSource& source) { s_source = source; }

bool Fast() { return s_fast.load() && s_state == State::Recording; }

void Frame(IDirect3DDevice9* device) {
	const State seen = s_state;
	if (seen != State::Recording && seen != State::Closing) return;
	std::lock_guard<std::mutex> lock(s_lock);
	if (s_state == State::Recording && !s_wanted) {
		// Asked to stop: the encoder closes the file on its own time. After a
		// Cancel it was already told to discard it, which Stop leaves as it is.
		grab::Release();
		EndFast();
		if (s_sending) { link::Stop(); s_state = State::Closing; }
		else s_state = State::Failed;
		s_sending = false;
	}
	else if (s_state == State::Recording) {
		if (!s_deviceAvailable) return;
		if (!s_opened) {
			// The first frame tells the picture's size.
			s_opened = true;
			unsigned width = 0, height = 0;
			// The sound is held once the encoder is open (Start waits for it
			// when fast), so the first picture and its sound start together.
			const bool fastAsked = s_fastAsked && s_source.hold && s_source.render && s_source.release;
			if (s_fastAsked && !fastAsked) spdlog::warn("Video: a fast export was asked, but nothing makes the game's sound by the picture; recording at the game's own speed");
			s_sending = grab::Open(device, width, height) && link::Start(s_file, width, height, s_encoder, fastAsked);
			if (!s_sending) { grab::Release(); s_wanted = false; s_state = State::Failed; return; }
			const bool fast = fastAsked && s_source.hold();
			if (fastAsked && !fast) {
				// The encoder waits for sound that will not come: no file.
				spdlog::warn("Video: a fast export was asked, but the game's sound output was not found; no video");
				link::Fail(); grab::Release(); s_wanted = false; s_sending = false; s_state = State::Closing;
				return;
			}
			s_pictures = 0;
			s_fast = fast;
			if (fast) spdlog::info("Video: a fast export, its sound made by the picture");
		}
		if (s_fast) {
			// This picture's share of the sound, sent with it.
			s_sound.clear();
			s_source.render(s_pictures + 1, s_sound);
			link::SetSound(s_sound.data(), static_cast<unsigned>(s_sound.size() / 2));
		}
		if (grab::Grab(device, link::Send)) {
			if (s_fast) s_pictures++;
		}
		else {
			EndFast();
			link::Fail();
			grab::Release(); s_wanted = false; s_sending = false;
			s_state = State::Closing;
		}
	}
	if (s_state == State::Closing) {
		const link::Result result = link::Poll();
		if (result != link::Result::Pending) s_state = result == link::Result::Done ? State::Done : State::Failed;
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
