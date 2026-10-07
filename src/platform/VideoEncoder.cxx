#include "VideoEncoder.hxx"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <codecapi.h>
#include <mmdeviceapi.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {
// Bits per pixel per frame at 60 frames a second: 11 Mbit/s at 720p, 25 at
// 1080p, 100 at 4K. Twice what YouTube asks for an upload, so its re-encode
// has room. HEVC needs about six tenths of that for the same picture.
constexpr double kBitsPerPixel = 0.2, kHevcBitsPerPixel = 0.12;
// Captured as 32-bit float, so sound the Windows mixer turned down is raised
// again without its low bits lost; written as 16-bit.
constexpr UINT32 kFrameRate = 60, kSampleRate = 48000, kChannels = 2, kAudioFrameBytes = 4, kCaptureFrameBytes = 8;
constexpr UINT32 kAacBytesPerSecond = 24000; // 192 kbit/s, the AAC encoder's top rate

std::mutex s_lock;
ComPtr<IMFSinkWriter> s_writer;
DWORD s_videoStream = 0, s_audioStream = 0;
UINT32 s_width = 0, s_height = 0;
LONGLONG s_start = 0, s_lastFrame = -1, s_pictures = 0, s_dropped = 0;
bool s_failed = false;
// Pictures with the writer, and how many it may hold before the next is
// dropped: its own queue of about 65 and then as many as memory allows.
std::atomic<long> s_held{0};
long s_holdLimit = 0;
std::string s_summary;

// A tracked sample calls this on its last release, from whichever thread let go of it.
struct Returned : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFAsyncCallback> {
	STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
	STDMETHODIMP Invoke(IMFAsyncResult* result) override {
		ComPtr<IUnknown> object; ComPtr<IMFSample> sample;
		if (SUCCEEDED(result->GetObject(&object)) && SUCCEEDED(object.As(&sample))) sample->RemoveAllBuffers();
		s_held--;
		return S_OK;
	}
};
ComPtr<Returned> s_returned;
ComPtr<IAudioClient> s_client;
ComPtr<IAudioCaptureClient> s_capture;
std::thread s_audio;
DWORD s_soundPid = 0;
std::atomic<bool> s_audioRuns{false};

LONGLONG Now() { return sf4e::platform::video::Clock(); }

struct Activated : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, Microsoft::WRL::FtmBase, IActivateAudioInterfaceCompletionHandler> {
	HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	~Activated() { CloseHandle(done); }
	STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override { SetEvent(done); return S_OK; }
};

// The process's volume in the Windows mixer, or null while it has no sound
// session. Loopback hands the sound over after that volume is applied.
// On the default output device only, where the game plays.
ComPtr<ISimpleAudioVolume> MixerVolume(DWORD pid) {
	ComPtr<IMMDeviceEnumerator> devices; ComPtr<IMMDevice> device; ComPtr<IAudioSessionManager2> manager; ComPtr<IAudioSessionEnumerator> sessions;
	int count = 0;
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) ||
		FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
		FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(manager.GetAddressOf()))) ||
		FAILED(manager->GetSessionEnumerator(&sessions)) || FAILED(sessions->GetCount(&count))) return nullptr;
	for (int i = 0; i < count; i++) {
		ComPtr<IAudioSessionControl> control; ComPtr<IAudioSessionControl2> session; ComPtr<ISimpleAudioVolume> volume; DWORD owner = 0;
		if (SUCCEEDED(sessions->GetSession(i, &control)) && SUCCEEDED(control.As(&session)) && SUCCEEDED(session->GetProcessId(&owner)) &&
			owner == pid && SUCCEEDED(control.As(&volume))) return volume;
	}
	return nullptr;
}

// One process's sound alone (not Discord's, not the desktop's), as 48 kHz float stereo.
bool OpenSound(DWORD pid) {
	s_soundPid = pid;
	const HMODULE devices = LoadLibraryW(L"MMDevAPI.dll");
	if (!devices || !GetProcAddress(devices, "ActivateAudioInterfaceAsync")) return false;
	AUDIOCLIENT_ACTIVATION_PARAMS params = {};
	params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
	params.ProcessLoopbackParams.TargetProcessId = pid;
	params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
	PROPVARIANT blob = {};
	blob.vt = VT_BLOB; blob.blob.cbSize = sizeof params; blob.blob.pBlobData = reinterpret_cast<BYTE*>(&params);
	const auto handler = Microsoft::WRL::Make<Activated>();
	ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
	HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &blob, handler.Get(), &operation);
	ComPtr<IUnknown> unknown;
	HRESULT activated = E_FAIL;
	if (SUCCEEDED(hr) && WaitForSingleObject(handler->done, 5000) != WAIT_OBJECT_0) hr = E_ABORT;
	if (SUCCEEDED(hr)) hr = operation->GetActivateResult(&activated, &unknown);
	if (SUCCEEDED(hr)) hr = activated;
	if (SUCCEEDED(hr)) hr = unknown.As(&s_client);
	WAVEFORMATEX format = {WAVE_FORMAT_IEEE_FLOAT, static_cast<WORD>(kChannels), kSampleRate, kSampleRate * kCaptureFrameBytes, static_cast<WORD>(kCaptureFrameBytes), 32, 0};
	// Two seconds of buffer: the capture thread sleeps ten milliseconds at a time.
	if (SUCCEEDED(hr)) hr = s_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 20000000, 0, &format, nullptr);
	if (SUCCEEDED(hr)) hr = s_client->GetService(IID_PPV_ARGS(&s_capture));
	if (FAILED(hr)) { spdlog::warn("Video: no sound capture ({:#x}); the file will be silent", static_cast<unsigned>(hr)); s_capture.Reset(); s_client.Reset(); }
	return SUCCEEDED(hr);
}

// On the capture thread, the only one that writes sound. data is the
// captured float sound, or null for silence; gain brings it back to full.
void WriteSound(const BYTE* data, UINT32 frames, float gain, LONGLONG& written) {
	ComPtr<IMFMediaBuffer> buffer; ComPtr<IMFSample> sample; BYTE* bytes = nullptr;
	const DWORD size = frames * kAudioFrameBytes;
	if (FAILED(MFCreateMemoryBuffer(size, &buffer)) || FAILED(buffer->Lock(&bytes, nullptr, nullptr))) return;
	if (data) {
		const float* in = reinterpret_cast<const float*>(data);
		short* out = reinterpret_cast<short*>(bytes);
		for (UINT32 i = 0; i < frames * kChannels; i++) out[i] = static_cast<short>((std::max)(-1.f, (std::min)(1.f, in[i] * gain)) * 32767);
	}
	else memset(bytes, 0, size);
	buffer->Unlock(); buffer->SetCurrentLength(size);
	if (FAILED(MFCreateSample(&sample))) return;
	sample->AddBuffer(buffer.Get());
	sample->SetSampleTime(written * 10000000 / kSampleRate);
	sample->SetSampleDuration(static_cast<LONGLONG>(frames) * 10000000 / kSampleRate);
	s_writer->WriteSample(s_audioStream, sample.Get());
	written += frames;
}

void CaptureSound() {
	LONGLONG written = 0;
	// The file gets the game at full volume however far the Windows mixer has
	// it turned down: the level is read ten times a second and divided out.
	const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	ComPtr<ISimpleAudioVolume> mixer;
	float gain = 1, lastLevel = -1;
	for (int turn = 0; s_audioRuns; turn++) {
		if (!mixer && turn % 100 == 0) mixer = MixerVolume(s_soundPid);
		float level = 1; BOOL muted = FALSE;
		if (mixer && turn % 10 == 0 && SUCCEEDED(mixer->GetMasterVolume(&level)) && SUCCEEDED(mixer->GetMute(&muted))) {
			if (muted) level = 0;
			gain = level > 0 ? 1 / level : 1;
			if (level != lastLevel) {
				if (level <= 0) spdlog::warn("Video: the game is muted in the Windows mixer, so the file is silent from here");
				else if (level < 1) spdlog::info("Video: the game is at {:.0f}% in the Windows mixer; its sound is raised to full for the file", level * 100);
				lastLevel = level;
			}
		}
		UINT32 packet = 0;
		while (SUCCEEDED(s_capture->GetNextPacketSize(&packet)) && packet) {
			BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 at = 0;
			if (FAILED(s_capture->GetBuffer(&data, &frames, &flags, nullptr, &at))) break;
			// Loopback delivers nothing while nothing plays, and the AAC encoder
			// takes its input as one unbroken run: a gap is written as silence, or
			// the sound after it would come early against the picture.
			const LONGLONG due = ((at ? static_cast<LONGLONG>(at) : Now()) - s_start) * kSampleRate / 10000000;
			if (due > written + kSampleRate / 50) WriteSound(nullptr, static_cast<UINT32>(due - written), 1, written);
			WriteSound(flags & AUDCLNT_BUFFERFLAGS_SILENT ? nullptr : data, frames, gain, written);
			s_capture->ReleaseBuffer(frames);
		}
		Sleep(10);
	}
	mixer.Reset();
	if (SUCCEEDED(com)) CoUninitialize();
}

HRESULT VideoType(const GUID& format, IMFMediaType** out) {
	ComPtr<IMFMediaType> type;
	HRESULT hr = MFCreateMediaType(&type);
	if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, format);
	if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, s_width, s_height);
	if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, kFrameRate, 1);
	if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	// What Frame is given, so a player turns it back into the game's colours.
	if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
	if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
	if (SUCCEEDED(hr)) *out = type.Detach();
	return hr;
}

HRESULT AudioType(const GUID& format, UINT32 bytesPerSecond, IMFMediaType** out) {
	ComPtr<IMFMediaType> type;
	HRESULT hr = MFCreateMediaType(&type);
	if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, format);
	if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
	if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
	if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kAudioFrameBytes);
	if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSecond);
	if (SUCCEEDED(hr)) *out = type.Detach();
	return hr;
}

// The graphics card to encode on, and its name. Media Foundation left to
// itself takes the first hardware encoder registered, which on a laptop with
// two cards is the built-in one's, whichever card the game draws on; and this
// process, unknown to the driver, would be given the built-in card too. Asked
// with a device on the card Windows calls high performance, which is the
// discrete one where there are two, the sink writer takes that card's
// encoder. Null where Windows cannot say (before 10 1803), the card is a
// software one, or SF4E_VIDEO_ENCODER=windows asks for the old choice.
ComPtr<IMFDXGIDeviceManager> s_card;
std::string s_cardName;
ComPtr<IMFDXGIDeviceManager> PreferredCard(std::string& name) {
	ComPtr<IDXGIFactory6> factory; ComPtr<IDXGIAdapter1> adapter; ComPtr<ID3D11Device> device; ComPtr<ID3D10Multithread> threads; ComPtr<IMFDXGIDeviceManager> manager;
	DXGI_ADAPTER_DESC1 card = {}; UINT token = 0; char asked[16] = {0};
	if (GetEnvironmentVariableA("SF4E_VIDEO_ENCODER", asked, sizeof asked) && !_stricmp(asked, "windows")) return nullptr;
	// Delay-loaded (CMakeLists.txt), like Media Foundation.
	if (!LoadLibraryW(L"dxgi.dll") || !LoadLibraryW(L"d3d11.dll")) return nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))) ||
		FAILED(adapter->GetDesc1(&card)) || (card.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) return nullptr;
	if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, nullptr))) return nullptr;
	// The encoder uses the device from its own threads.
	if (SUCCEEDED(device.As(&threads))) threads->SetMultithreadProtected(TRUE);
	if (FAILED(MFCreateDXGIDeviceManager(&token, &manager)) || FAILED(manager->ResetDevice(device.Get(), token))) return nullptr;
	char narrow[256] = {0};
	WideCharToMultiByte(CP_UTF8, 0, card.Description, -1, narrow, sizeof narrow - 1, nullptr, nullptr);
	name = narrow;
	return manager;
}

HRESULT Open(const std::wstring& file, IMFDXGIDeviceManager* card) {
	ComPtr<IMFAttributes> attributes;
	ComPtr<IMFMediaType> coded, nv12, aac, pcm;
	// Past this size the cards' H.264 encoders decline and Windows hands the
	// job to whatever else is registered (one such took 1.4 GB for 3840x2400).
	const bool hevc = s_width > 4096 || s_height > 2160;
	HRESULT hr = MFCreateAttributes(&attributes, 2);
	// The graphics card's encoder, when its driver registered one.
	if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
	if (SUCCEEDED(hr) && card) hr = attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, card);
	if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(file.c_str(), nullptr, attributes.Get(), &s_writer);
	if (SUCCEEDED(hr)) hr = VideoType(hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264, &coded);
	if (SUCCEEDED(hr)) hr = coded->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(s_width * s_height * kFrameRate * (hevc ? kHevcBitsPerPixel : kBitsPerPixel)));
	if (SUCCEEDED(hr) && !hevc) hr = coded->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
	if (SUCCEEDED(hr)) hr = s_writer->AddStream(coded.Get(), &s_videoStream);
	if (SUCCEEDED(hr)) hr = VideoType(MFVideoFormat_NV12, &nv12);
	if (SUCCEEDED(hr)) hr = nv12->SetUINT32(MF_MT_DEFAULT_STRIDE, s_width);
	if (SUCCEEDED(hr)) hr = s_writer->SetInputMediaType(s_videoStream, nv12.Get(), nullptr);
	if (SUCCEEDED(hr) && s_capture) {
		hr = AudioType(MFAudioFormat_AAC, kAacBytesPerSecond, &aac);
		if (SUCCEEDED(hr)) hr = s_writer->AddStream(aac.Get(), &s_audioStream);
		if (SUCCEEDED(hr)) hr = AudioType(MFAudioFormat_PCM, kSampleRate * kAudioFrameBytes, &pcm);
		if (SUCCEEDED(hr)) hr = s_writer->SetInputMediaType(s_audioStream, pcm.Get(), nullptr);
	}
	if (SUCCEEDED(hr)) hr = s_writer->BeginWriting();
	return hr;
}

// Which encoder the sink writer took, for the log: the export's name promises the graphics card's.
void LogEncoder() {
	ComPtr<IMFTransform> encoder; ComPtr<IMFAttributes> attributes;
	wchar_t name[128] = L"unnamed"; UINT32 async = 0;
	if (FAILED(s_writer->GetServiceForStream(s_videoStream, GUID_NULL, IID_PPV_ARGS(&encoder))) || FAILED(encoder->GetAttributes(&attributes))) return;
	attributes->GetString(MFT_FRIENDLY_NAME_Attribute, name, 128, nullptr);
	// A driver's encoder may give no name; its vendor ("VEN_8086" is Intel) tells whose it is.
	if (!wcscmp(name, L"unnamed")) attributes->GetString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, name, 128, nullptr);
	attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async);
	char narrow[256] = {0};
	WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof narrow - 1, nullptr, nullptr);
	s_summary = fmt::format("{}x{} {} by '{}' ({}{}), holding up to {} pictures{}", s_width, s_height, s_width > 4096 || s_height > 2160 ? "HEVC" : "H.264", narrow, async ? "hardware" : "software",
		s_card ? " on " + s_cardName : std::string(", Windows' choice"), s_holdLimit, s_capture ? "" : ", no sound");
	spdlog::info("Video: {}", s_summary);
}
}

namespace sf4e { namespace platform { namespace video {

long long Clock() {
	LARGE_INTEGER counter, frequency;
	QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency);
	return counter.QuadPart / frequency.QuadPart * 10000000 + counter.QuadPart % frequency.QuadPart * 10000000 / frequency.QuadPart;
}

const std::string& Summary() { return s_summary; }

bool Begin(const std::wstring& file, unsigned width, unsigned height, unsigned long soundPid) {
	std::lock_guard<std::mutex> lock(s_lock);
	// Delay-loaded (CMakeLists.txt): N editions without the Media Feature Pack have neither.
	if (s_writer || !width || !height || width % 2 || height % 2 || !LoadLibraryW(L"mfplat.dll") || !LoadLibraryW(L"mfreadwrite.dll")) return false;
	HRESULT hr = MFStartup(MF_VERSION);
	if (FAILED(hr)) { spdlog::warn("Video: Media Foundation did not start ({:#x})", static_cast<unsigned>(hr)); return false; }
	s_width = width; s_height = height; s_lastFrame = -1; s_pictures = s_dropped = 0; s_failed = false;
	s_summary.clear(); s_held = 0;
	// Six tenths of the address space still free, in pictures; 120 is two seconds, more than any queue needs.
	MEMORYSTATUSEX memory = {sizeof memory};
	GlobalMemoryStatusEx(&memory);
	s_holdLimit = static_cast<long>((std::min)(120ull, memory.ullAvailVirtual * 6 / 10 / (width * height * 3ull / 2)));
	OpenSound(soundPid);
	// On the preferred card first; where that card has no encoder for this
	// picture, the encoder Windows picks by itself, as before.
	s_card = PreferredCard(s_cardName);
	hr = s_card ? Open(file, s_card.Get()) : E_NOINTERFACE;
	if (FAILED(hr)) {
		if (s_card) spdlog::info("Video: no encoder on {} ({:#x}); taking the one Windows picks", s_cardName, static_cast<unsigned>(hr));
		s_writer.Reset(); s_card.Reset(); s_cardName.clear();
		DeleteFileW(file.c_str());
		hr = Open(file, nullptr);
	}
	s_returned = Microsoft::WRL::Make<Returned>();
	if (FAILED(hr)) {
		s_summary = fmt::format("the encoder did not open ({:#x})", static_cast<unsigned>(hr));
		spdlog::warn("Video: {}", s_summary);
		s_writer.Reset(); s_card.Reset(); s_capture.Reset(); s_client.Reset(); MFShutdown();
		DeleteFileW(file.c_str());
		return false;
	}
	LogEncoder();
	s_start = Now();
	if (s_capture && SUCCEEDED(s_client->Start())) { s_audioRuns = true; s_audio = std::thread(CaptureSound); }
	return true;
}

void Frame(const void* luma, int lumaPitch, const void* chroma, int chromaPitch, long long at) {
	std::lock_guard<std::mutex> lock(s_lock);
	if (!s_writer || s_failed) return;
	ComPtr<IMFTrackedSample> tracked; ComPtr<IMFSample> sample; ComPtr<IMFMediaBuffer> buffer; BYTE* bytes = nullptr;
	const DWORD size = s_width * s_height * 3 / 2;
	if (s_held >= s_holdLimit || FAILED(MFCreateMemoryBuffer(size, &buffer)) || FAILED(MFCreateTrackedSample(&tracked)) || FAILED(tracked.As(&sample)) ||
		FAILED(buffer->Lock(&bytes, nullptr, nullptr))) { s_dropped++; return; }
	MFCopyImage(bytes, s_width, static_cast<const BYTE*>(luma), lumaPitch, s_width, s_height);
	MFCopyImage(bytes + s_width * s_height, s_width, static_cast<const BYTE*>(chroma), chromaPitch, s_width, s_height / 2);
	buffer->Unlock(); buffer->SetCurrentLength(size);
	sample->AddBuffer(buffer.Get());
	// From here the sample's last release is counted (Returned).
	s_held++;
	tracked->SetAllocator(s_returned.Get(), nullptr);
	// The real time it was drawn at, so the picture stays with the sound when a frame is missing.
	at = (std::max)(at - s_start, s_lastFrame + 1);
	s_lastFrame = at;
	sample->SetSampleTime(at);
	sample->SetSampleDuration(10000000 / kFrameRate);
	const HRESULT hr = s_writer->WriteSample(s_videoStream, sample.Get());
	if (SUCCEEDED(hr)) s_pictures++;
	else if (!s_failed) { s_failed = true; spdlog::warn("Video: the encoder stopped taking pictures after {} ({:#x})", s_pictures, static_cast<unsigned>(hr)); }
}

bool End() {
	std::lock_guard<std::mutex> lock(s_lock);
	if (!s_writer) return false;
	if (s_audioRuns.exchange(false)) { s_audio.join(); s_client->Stop(); }
	const HRESULT hr = s_writer->Finalize();
	s_summary = fmt::format("{} pictures written, {} dropped with the encoder behind, closed with {:#x}", s_pictures, s_dropped, static_cast<unsigned>(hr));
	spdlog::info("Video: {}", s_summary);
	s_writer.Reset(); s_card.Reset(); s_capture.Reset(); s_client.Reset();
	MFShutdown();
	return SUCCEEDED(hr) && s_pictures > 0 && !s_failed;
}

} } }
