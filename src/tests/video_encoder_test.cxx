// The video export end to end, without the game: this executable starts
// itself as the encoder's process (as the game starts Launcher.exe), sends two
// seconds of a test picture (white over black) over the link while it plays a
// tone, and checks the file. 640x360 and three seconds, or the size and the
// frame count given after the file name.
// Skips (77) where Windows has no encoder for it.
#include "../platform/VideoLink.hxx"
#include "../platform/VideoLinkProtocol.hxx"
#include "../platform/VideoLinkServe.hxx"
#include "../platform/VideoTemporary.hxx"
#include "../sf4e/sf4e__ReplayCapture.hxx"
#include "test_support.hxx"

#include <windows.h>
#include <mmsystem.h>
#include <d3d9.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <vector>

namespace {
namespace link = sf4e::platform::videolink;

// The longest a Poll took, for the game thread that calls it each frame.
ULONGLONG s_longestPoll = 0;

// Stop, then Poll as the game does each frame until the encoder's process has
// closed the file: true when it holds a video.
bool Finish() {
	link::Stop();
	for (;; Sleep(16)) {
		const ULONGLONG asked = GetTickCount64();
		const link::Result result = link::Poll();
		s_longestPoll = (std::max)(s_longestPoll, GetTickCount64() - asked);
		if (result != link::Result::Pending) return result == link::Result::Done;
	}
}

// An encoder that never closes its file, for the game's deadline: it holds
// the temporary file open, without letting it be removed, until it is ended.
int Hang(const std::wstring& name) {
	const HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
	const auto* shared = mapping ? static_cast<const link::Shared*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(link::Shared))) : nullptr;
	if (!shared) return 2;
	std::wstring path(shared->file, wcsnlen(shared->file, 1024));
	if (CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE) return 2;
	Sleep(INFINITE);
	return 1;
}

// A game gone by the time its encoder's process looks: the link's shared
// memory names a temporary file reserved beside file, but the wake event
// (without wake) or the game's process (game) is not there. The encoder's
// exit code, or -1 when it was still running ten seconds on.
int Orphaned(const std::wstring& self, const std::filesystem::path& file, bool wake, DWORD game) {
	const std::wstring name = L"Local\\sf4e-video-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
	const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(link::Shared), name.c_str());
	auto* const shared = mapping ? static_cast<link::Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
	CHECK(shared);
	std::wstring temporary;
	CHECK(link::ReserveTemporary(file.wstring(), temporary));
	shared->width = 640; shared->height = 360; shared->game = game;
	wcscpy_s(shared->file, temporary.c_str()); wcscpy_s(shared->final, file.c_str());
	const HANDLE event = wake ? CreateEventW(nullptr, FALSE, FALSE, link::WakeName(name).c_str()) : nullptr;
	std::wstring command = L"\"" + self + L"\" --encode-video " + name;
	STARTUPINFOW startup = {sizeof startup}; PROCESS_INFORMATION process = {};
	CHECK(CreateProcessW(self.c_str(), &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
	DWORD code = static_cast<DWORD>(-1);
	if (WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
	else { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000); }
	CloseHandle(process.hThread); CloseHandle(process.hProcess);
	if (event) CloseHandle(event);
	UnmapViewOfFile(shared); CloseHandle(mapping);
	return static_cast<int>(code);
}

// What is left of an export's temporary files beside file; held counts
// those a process has open without letting them be removed.
int Temporaries(const std::filesystem::path& file, int* held = nullptr) {
	int count = 0;
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(file.parent_path(), error)) {
		const std::wstring name = entry.path().filename().wstring();
		if (name.rfind(file.filename().wstring() + L".", 0) != 0 || entry.path().extension() != L".sf4e-video-tmp") continue;
		count++;
		const HANDLE probe = CreateFileW(entry.path().c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
		else if (held && GetLastError() == ERROR_SHARING_VIOLATION) ++*held;
	}
	return count;
}

// A game that cancels its export through the capture
// (sf4e__ReplayCapture.hxx) and then draws no frame again: it ends at once,
// as when the player cancels and closes the game. First a cancel before the
// first frame, which starts no encoder at all. 77 without a Direct3D 9
// device; 2 or 3 when the capture misbehaves.
int CancelledGame(const std::wstring& self, const std::filesystem::path& file) {
	namespace capture = sf4e::replaycapture;
	const HWND window = CreateWindowA("STATIC", "video export cancel test", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	IDirect3DDevice9* device = nullptr;
	D3DPRESENT_PARAMETERS p = {};
	p.Windowed = TRUE; p.SwapEffect = D3DSWAPEFFECT_DISCARD; p.BackBufferFormat = D3DFMT_X8R8G8B8; p.BackBufferWidth = 640; p.BackBufferHeight = 360; p.hDeviceWindow = window;
	if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &p, &device))) return 77;
	const int before = Temporaries(file);
	capture::Begin(file.wstring(), self);
	if (!capture::Cancel() || capture::GetState() != capture::State::Failed) return 2;
	capture::Frame(device);
	if (capture::GetState() != capture::State::Failed || Temporaries(file) != before) return 2;
	capture::Clear();
	// Two seconds of pictures, so the encoder has opened and has a video to
	// save, then the cancel, and the game is gone without another frame.
	capture::Begin(file.wstring(), self);
	for (int frame = 0; frame < 120; frame++) {
		device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(frame * 2, 0, 255 - frame * 2), 1.0f, 0);
		capture::Frame(device);
		Sleep(17);
	}
	if (capture::GetState() != capture::State::Recording) return 3;
	if (!capture::Cancel()) return 2;
	ExitProcess(0);
}
}

int wmain(int argc, wchar_t** argv) {
	if (argc == 3 && !wcscmp(argv[1], L"--encode-video")) {
		if (std::getenv("SF4E_TEST_ENCODER_HANGS")) return Hang(argv[2]);
		return link::Serve(argv[2]);
	}
	if (argc == 3 && !wcscmp(argv[1], L"--cancelled-export")) {
		wchar_t game[MAX_PATH] = {0};
		GetModuleFileNameW(nullptr, game, MAX_PATH);
		return CancelledGame(game, argv[2]);
	}
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
	const int leftBefore = Temporaries(file);
	if (!link::Start(file.wstring(), width, height, self)) { std::cerr << "The encoder's process did not start\n"; return 1; }
	// Not stopped yet: no deadline runs, whatever the encoder is doing.
	if (link::Poll() != link::Result::Pending) { std::cerr << "An export ended before it was stopped\n"; return 1; }
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

	const bool made = Finish();
	if (link::Poll() != link::Result::Failed || Finish()) { std::cerr << "A finished export finished again\n"; return 1; }
	CHECK(Temporaries(file) == leftBefore);
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
	CHECK(!Finish());
	CHECK(read(file) == previous);
	// The failed export's temporary file is gone with it.
	CHECK(Temporaries(file) == leftBefore);
	// A player has the earlier video open without letting it be replaced: the
	// new one cannot take its name, and the encoder's process removes its file
	// itself, since the game may be gone. No Poll until then, which would.
	CHECK(link::Start(file.wstring(), width, height, self));
	const HANDLE watching = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	CHECK(watching != INVALID_HANDLE_VALUE);
	for (int frame = 0; frame < 120; frame++) { link::Send(luma.data(), pitch, chroma.data(), pitch); Sleep(17); }
	link::Stop();
	for (const ULONGLONG asked = GetTickCount64(); Temporaries(file) != leftBefore && GetTickCount64() - asked < 20000;) Sleep(50);
	CHECK(Temporaries(file) == leftBefore);
	CHECK(!Finish());
	CloseHandle(watching);
	CHECK(read(file) == previous);
	// The game gone before its encoder's process found the wake event, or the
	// game's process: that process ends at once and removes the temporary file
	// itself, and the earlier video stays.
	const int noWake = Orphaned(self, file, false, GetCurrentProcessId()), noGame = Orphaned(self, file, true, 0x7FFFFFFC);
	CHECK(noWake != -1 && noWake != 0 && noGame != -1 && noGame != 0);
	CHECK(Temporaries(file) == leftBefore);
	CHECK(read(file) == previous);
	// A game cancels its export and is gone without drawing again: the
	// encoder still discards the video, the earlier one stays, and the
	// temporary file goes with the encoder's process.
	{
		std::wstring command = L"\"" + std::wstring(self) + L"\" --cancelled-export \"" + file.wstring() + L"\"";
		STARTUPINFOW startup = {sizeof startup}; PROCESS_INFORMATION game = {};
		CHECK(CreateProcessW(self, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &game));
		DWORD code = 1;
		CHECK(WaitForSingleObject(game.hProcess, 30000) == WAIT_OBJECT_0 && GetExitCodeProcess(game.hProcess, &code));
		CloseHandle(game.hThread); CloseHandle(game.hProcess);
		if (code == 77) std::cout << "No Direct3D 9 device here; the cancelled export was skipped\n";
		else {
			CHECK(code == 0);
			for (const ULONGLONG asked = GetTickCount64(); Temporaries(file) != leftBefore && GetTickCount64() - asked < 20000;) Sleep(50);
			CHECK(Temporaries(file) == leftBefore);
			CHECK(read(file) == previous);
		}
	}
	// An encoder that does not close the file is ended 30 seconds after Stop
	// without Poll waiting for it; its temporary file is removed once it has
	// gone, and the earlier video kept.
	SetEnvironmentVariableW(L"SF4E_TEST_ENCODER_HANGS", L"1");
	CHECK(link::Start(file.wstring(), width, height, self));
	SetEnvironmentVariableW(L"SF4E_TEST_ENCODER_HANGS", nullptr);
	int held = 0;
	for (const ULONGLONG asked = GetTickCount64(); !held && GetTickCount64() - asked < 10000; Sleep(50)) { held = 0; Temporaries(file, &held); }
	CHECK(held == 1);
	const ULONGLONG stopped = GetTickCount64();
	s_longestPoll = 0;
	CHECK(!Finish());
	CHECK(GetTickCount64() - stopped >= 30000);
	CHECK(s_longestPoll < 500);
	CHECK(Temporaries(file) == leftBefore);
	CHECK(read(file) == previous);
	std::wcout << L"Encoded " << size << L" bytes to " << file.wstring() << L"\n";
	return 0;
}
