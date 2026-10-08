// The video export end to end, without the game: this executable starts
// itself as the encoder's process (as the game starts Launcher.exe), sends two
// seconds of a test picture (white over black) over the link while it plays a
// tone, and checks the file. 640x360 and three seconds, or the size and the
// frame count given after the file name.
// Then a second export of the same file that is stopped without one: the
// first stays as it was, and neither leaves its temporary file behind.
// Skips (77) only where Windows has no encoder for the picture.
#include "../platform/VideoLink.hxx"
#include "../platform/VideoServe.hxx"

#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

namespace link = sf4e::platform::videolink;

// Stop, then up to 30 seconds for the encoder's process to end, as the game's
// tick waits for it (sf4e__ReplayCapture.cxx). How it ended.
static link::Result Finish(bool keep) {
	link::Result result = link::NoLink;
	link::Stop(keep);
	for (int waited = 0; waited < 3000; waited++) { if (link::Closed(result)) return result; Sleep(10); }
	link::Abandoned stuck = link::Abandon();
	link::End(stuck);
	return link::NoLink;
}

// The folder holds a temporary file of an export of that video.
static bool LeftBehind(const std::filesystem::path& file) {
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(file.parent_path(), error))
		if (entry.path().filename().wstring().rfind(file.filename().wstring() + L".", 0) == 0) return true;
	return false;
}

int wmain(int argc, wchar_t** argv) {
	if (argc == 3 && !wcscmp(argv[1], L"--encode-video")) return link::Serve(argv[2]);
	const std::filesystem::path file = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() / L"ember-video-encoder-test.mp4";
	const unsigned width = argc > 3 ? _wtoi(argv[2]) : 640, height = argc > 3 ? _wtoi(argv[3]) : 360;
	const int frames = argc > 4 ? _wtoi(argv[4]) : 180;
	wchar_t self[MAX_PATH] = {0};
	GetModuleFileNameW(nullptr, self, MAX_PATH);
	std::error_code error;
	std::filesystem::remove(file, error);
	if (link::Start(file.wstring(), width, height, L"no-such-encoder.exe")) { std::cerr << "A missing encoder started\n"; return 1; }
	if (!link::Start(file.wstring(), width, height, self)) { std::cerr << "The encoder's process did not start\n"; return 1; }
	if (link::Start(file.wstring(), width, height, self)) { std::cerr << "A second export started over the first\n"; return 1; }

	// SF4E_TEST_GAP: after the first second no picture is sent for that many
	// seconds while the tone plays on, as when a fullscreen game is left with
	// Alt+Tab during an export: the file still has to close.
	const int gap = std::getenv("SF4E_TEST_GAP") ? std::atoi(std::getenv("SF4E_TEST_GAP")) : 0;
	// Three seconds of 440 Hz, and the gap's: the encoder's process has this one's sound to find.
	std::vector<std::int16_t> tone(static_cast<std::size_t>(48000) * (3 + gap) * 2);
	for (std::size_t i = 0; i < tone.size(); i++) tone[i] = static_cast<std::int16_t>(8000 * std::sin(i / 2 * 2 * 3.14159265 * 440 / 48000));
	WAVEFORMATEX format = {WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
	HWAVEOUT out = nullptr; WAVEHDR header = {};
	header.lpData = reinterpret_cast<LPSTR>(tone.data()); header.dwBufferLength = static_cast<DWORD>(tone.size() * 2);
	if (waveOutOpen(&out, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR) { waveOutPrepareHeader(out, &header, sizeof header); waveOutWrite(out, &header, sizeof header); }
	// Turned down to a tenth in the Windows mixer, or to the percent in
	// SF4E_TEST_MIXER_VOLUME: the file's tone is to be as loud as at full.
	using Microsoft::WRL::ComPtr;
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	ComPtr<IMMDeviceEnumerator> devices; ComPtr<IMMDevice> device; ComPtr<IAudioSessionManager> manager; ComPtr<ISimpleAudioVolume> mixer;
	const char* asked = std::getenv("SF4E_TEST_MIXER_VOLUME");
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) && SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
		SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(manager.GetAddressOf()))) &&
		SUCCEEDED(manager->GetSimpleAudioVolume(nullptr, FALSE, &mixer))) mixer->SetMasterVolume(asked ? std::atoi(asked) / 100.f : 0.1f, nullptr);

	// A wider pitch than the picture, as a locked Direct3D surface may have.
	const int pitch = width + 64;
	std::vector<std::uint8_t> luma(pitch * height), chroma(pitch * height / 2, 128);
	for (unsigned y = 0; y < height; y++) std::memset(&luma[y * pitch], y < height / 2 ? 235 : 16, width);
	// A frame every sixtieth of a second, as the game draws them; the first
	// second's are dropped while the encoder opens, as in the game.
	const DWORD start = GetTickCount();
	for (int frame = 0; frame < frames; frame++) {
		link::Send(luma.data(), pitch, chroma.data(), pitch);
		if (gap && frame == 60) Sleep(gap * 1000);
		while (GetTickCount() - start < static_cast<DWORD>((frame + 1) * 1000 / 60 + (frame >= 60 ? gap * 1000 : 0))) Sleep(1);
	}
	if (mixer) mixer->SetMasterVolume(1, nullptr);
	if (out) { waveOutReset(out); waveOutUnprepareHeader(out, &header, sizeof header); waveOutClose(out); }

	if (link::Lost()) { std::cerr << "The encoder's process ended during the export\n"; }
	const link::Result made = Finish(true);
	if (Finish(true) != link::NoLink) { std::cerr << "A finished export finished again\n"; return 1; }
	if (made == link::NotOpened) { std::cout << "No encoder here for this size; skipped\n"; return 77; }
	const auto size = std::filesystem::file_size(file, error);
	if (made != link::Written || error || size < 10000) { std::cerr << "The file is missing or empty (the encoder ended with " << made << ")\n"; return 1; }
	if (LeftBehind(file)) { std::cerr << "The export left its temporary file\n"; return 1; }

	// An export that is given up: no new file, the earlier one untouched.
	if (!link::Start(file.wstring(), width, height, self)) { std::cerr << "The second export did not start\n"; return 1; }
	for (int frame = 0; frame < 90; frame++) { link::Send(luma.data(), pitch, chroma.data(), pitch); Sleep(16); }
	const link::Result dropped = Finish(false);
	if (dropped != link::NoVideo) { std::cerr << "A given-up export ended with " << dropped << "\n"; return 1; }
	if (std::filesystem::file_size(file, error) != size || error) { std::cerr << "A given-up export changed the earlier video\n"; return 1; }
	if (LeftBehind(file)) { std::cerr << "A given-up export left its temporary file\n"; return 1; }

	// A folder that takes no file is told apart from a Windows without an encoder.
	const std::filesystem::path nowhere = file.parent_path() / L"ember-no-such-folder" / L"video.mp4";
	if (!link::Start(nowhere.wstring(), width, height, self)) { std::cerr << "The third export did not start\n"; return 1; }
	const link::Result unwritable = Finish(true);
	if (unwritable != link::NotWritable) { std::cerr << "An export into a missing folder ended with " << unwritable << "\n"; return 1; }
	std::wcout << L"Encoded " << size << L" bytes to " << file.wstring() << L"\n";
	return 0;
}
