// The video export end to end, without the game: this executable starts
// itself as the encoder's process (as the game starts Launcher.exe), sends two
// seconds of a test picture (white over black) over the link while it plays a
// tone, and checks the file. 640x360 and three seconds, or the size and the
// frame count given after the file name.
// Skips (77) where Windows has no encoder for it.
#include "../platform/VideoLink.hxx"
#include "../platform/VideoTemporary.hxx"
#include "test_support.hxx"

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
#include <fstream>
#include <iterator>
#include <iostream>
#include <vector>

int wmain(int argc, wchar_t** argv) {
	namespace link = sf4e::platform::videolink;
	if (argc == 3 && !wcscmp(argv[1], L"--encode-video")) return link::Serve(argv[2]);
	const std::filesystem::path file = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() / L"ember-video-encoder-test.mp4";
	const unsigned width = argc > 3 ? _wtoi(argv[2]) : 640, height = argc > 3 ? _wtoi(argv[3]) : 360;
	const int frames = argc > 4 ? _wtoi(argv[4]) : 180;
	wchar_t self[MAX_PATH] = {0};
	GetModuleFileNameW(nullptr, self, MAX_PATH);
	std::error_code error;
	// Reservations never use another replay's final export name, and a failed
	// process start removes its reservation while preserving an existing video.
	const std::filesystem::path folder = std::filesystem::temp_directory_path() / (L"ember-video-files-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
	CHECK(std::filesystem::create_directory(folder));
	const auto final = folder / L"set.mp4", other = folder / L"set.part.mp4";
	{ std::ofstream old(other, std::ios::binary); old << "previous video"; }
	std::wstring first, second;
	CHECK(link::ReserveTemporary(final.wstring(), first) && link::ReserveTemporary(final.wstring(), second));
	CHECK(first != second && std::filesystem::path(first).extension() != L".mp4" && std::filesystem::path(second).extension() != L".mp4");
	CHECK(std::filesystem::exists(first) && std::filesystem::exists(second));
	CHECK(std::filesystem::remove(first) && std::filesystem::remove(second));
	CHECK(!link::Start(final.wstring(), width, height, L"no-such-encoder.exe"));
	// With no encoder named, the install's Launcher.exe is looked up; none beside this test is a clean failure that reserves nothing.
	if (!std::filesystem::exists(std::filesystem::path(self).parent_path() / L"Launcher.exe")) { CHECK(!link::Start(final.wstring(), width, height)); }
	CHECK(std::filesystem::file_size(other) == 14);
	CHECK(std::distance(std::filesystem::directory_iterator(folder), std::filesystem::directory_iterator()) == 1);
	std::filesystem::remove_all(folder);
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

	const bool made = link::Finish();
	if (link::Finish()) { std::cerr << "A finished export finished again\n"; return 1; }
	if (!made && !std::filesystem::exists(file, error)) { std::cout << "No encoder here for this size; skipped\n"; return 77; }
	const auto size = std::filesystem::file_size(file, error);
	if (!made || error || size < 10000) { std::cerr << "The file is missing or empty\n"; return 1; }
	const auto read = [](const std::filesystem::path& path) {
		std::ifstream in(path, std::ios::binary);
		return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	};
	const auto previous = read(file);
	CHECK(link::Start(file.wstring(), width, height, self));
	for (int frame = 0; frame < 120; frame++) { link::Send(luma.data(), pitch, chroma.data(), pitch); Sleep(17); }
	link::Fail();
	CHECK(!link::Finish());
	CHECK(read(file) == previous);
	std::wcout << L"Encoded " << size << L" bytes to " << file.wstring() << L"\n";
	return 0;
}
