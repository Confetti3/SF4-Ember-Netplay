#include "VideoEncoder.hxx"
#include "VideoLinkProtocol.hxx"
#include "Utf8.hxx"

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

// A tracked sample calls this on its last release, from whichever thread let
// go of it, and that may be after its Encoder is gone: the count of pictures
// with the writer lives here, kept alive by the samples that hold it.
struct Returned : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFAsyncCallback> {
	std::atomic<long> held{0};
	STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
	STDMETHODIMP Invoke(IMFAsyncResult* result) override {
		ComPtr<IUnknown> object; ComPtr<IMFSample> sample;
		if (SUCCEEDED(result->GetObject(&object)) && SUCCEEDED(object.As(&sample))) sample->RemoveAllBuffers();
		held--;
		return S_OK;
	}
};

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

HRESULT VideoType(const GUID& format, UINT32 width, UINT32 height, IMFMediaType** out) {
	ComPtr<IMFMediaType> type;
	HRESULT hr = MFCreateMediaType(&type);
	if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, format);
	if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
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
	name = sf4e::platform::WideToUtf8(card.Description);
	return manager;
}
}

namespace sf4e { namespace platform { namespace video {

struct Encoder::Impl {
	ComPtr<IMFSinkWriter> writer;
	DWORD videoStream = 0, audioStream = 0;
	UINT32 width = 0, height = 0;
	LONGLONG start = 0, lastFrame = -1, pictures = 0, dropped = 0;
	std::atomic<bool> failed{false};
	// Pictures with the writer (returned->held), and how many it may hold
	// before the next is dropped: its own queue of about 65 and then as many
	// as memory allows.
	ComPtr<Returned> returned;
	long holdLimit = 0;
	std::string summary;
	ComPtr<IAudioClient> client;
	ComPtr<IAudioCaptureClient> capture;
	std::thread audio;
	DWORD soundPid = 0;
	std::atomic<bool> audioRuns{false};
	ComPtr<IMFDXGIDeviceManager> card;
	std::string cardName;

	bool OpenSound(DWORD pid);
	bool WriteSound(const BYTE* data, UINT32 frames, float gain, LONGLONG& written);
	void CaptureSound();
	HRESULT Open(const std::wstring& file, IMFDXGIDeviceManager* on);
	void LogEncoder();
};

// One process's sound alone (not Discord's, not the desktop's), as 48 kHz float stereo.
bool Encoder::Impl::OpenSound(DWORD pid) {
	soundPid = pid;
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
	if (SUCCEEDED(hr)) hr = unknown.As(&client);
	WAVEFORMATEX format = {WAVE_FORMAT_IEEE_FLOAT, static_cast<WORD>(kChannels), kSampleRate, kSampleRate * kCaptureFrameBytes, static_cast<WORD>(kCaptureFrameBytes), 32, 0};
	// Two seconds of buffer: the capture thread sleeps ten milliseconds at a time.
	if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM, 20000000, 0, &format, nullptr);
	if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&capture));
	if (FAILED(hr)) { spdlog::warn("Video: no sound capture ({:#x}); the file will be silent", static_cast<unsigned>(hr)); capture.Reset(); client.Reset(); }
	return SUCCEEDED(hr);
}

// On the capture thread, the only one that writes sound. data is the
// captured float sound, or null for silence; gain brings it back to full.
bool Encoder::Impl::WriteSound(const BYTE* data, UINT32 frames, float gain, LONGLONG& written) {
	ComPtr<IMFMediaBuffer> buffer; ComPtr<IMFSample> sample; BYTE* bytes = nullptr;
	const DWORD size = frames * kAudioFrameBytes;
	if (FAILED(MFCreateMemoryBuffer(size, &buffer)) || FAILED(buffer->Lock(&bytes, nullptr, nullptr))) { failed = true; return false; }
	if (data) {
		const float* in = reinterpret_cast<const float*>(data);
		short* out = reinterpret_cast<short*>(bytes);
		for (UINT32 i = 0; i < frames * kChannels; i++) out[i] = static_cast<short>((std::max)(-1.f, (std::min)(1.f, in[i] * gain)) * 32767);
	}
	else memset(bytes, 0, size);
	if (FAILED(buffer->Unlock()) || FAILED(buffer->SetCurrentLength(size)) || FAILED(MFCreateSample(&sample)) ||
		FAILED(sample->AddBuffer(buffer.Get())) || FAILED(sample->SetSampleTime(written * 10000000 / kSampleRate)) ||
		FAILED(sample->SetSampleDuration(static_cast<LONGLONG>(frames) * 10000000 / kSampleRate)) ||
		FAILED(writer->WriteSample(audioStream, sample.Get()))) { failed = true; return false; }
	written += frames;
	return true;
}

void Encoder::Impl::CaptureSound() {
	LONGLONG written = 0;
	// The file gets the game at full volume however far the Windows mixer has
	// it turned down: the level is read ten times a second and divided out.
	const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	ComPtr<ISimpleAudioVolume> mixer;
	float gain = 1, lastLevel = -1;
	for (int turn = 0; audioRuns; turn++) {
		if (!mixer && turn % 100 == 0) mixer = MixerVolume(soundPid);
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
		while (!failed) {
			if (FAILED(capture->GetNextPacketSize(&packet))) { failed = true; break; }
			if (!packet) break;
			BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0; UINT64 at = 0;
			if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, &at))) { failed = true; break; }
			// Loopback delivers nothing while nothing plays, and the AAC encoder
			// takes its input as one unbroken run: a gap is written as silence, or
			// the sound after it would come early against the picture.
			const LONGLONG due = ((at ? static_cast<LONGLONG>(at) : Clock()) - start) * kSampleRate / 10000000;
			bool ok = true;
			if (due > written + kSampleRate / 50) ok = WriteSound(nullptr, static_cast<UINT32>(due - written), 1, written);
			if (ok) ok = WriteSound(flags & AUDCLNT_BUFFERFLAGS_SILENT ? nullptr : data, frames, gain, written);
			if (FAILED(capture->ReleaseBuffer(frames)) || !ok) failed = true;
		}
		Sleep(10);
	}
	mixer.Reset();
	if (SUCCEEDED(com)) CoUninitialize();
}

HRESULT Encoder::Impl::Open(const std::wstring& file, IMFDXGIDeviceManager* on) {
	ComPtr<IMFAttributes> attributes;
	ComPtr<IMFMediaType> coded, nv12, aac, pcm;
	// Past this size the cards' H.264 encoders decline and Windows hands the
	// job to whatever else is registered (one such took 1.4 GB for 3840x2400).
	const bool hevc = width > 4096 || height > 2160;
	HRESULT hr = MFCreateAttributes(&attributes, 2);
	if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
	// The graphics card's encoder, when its driver registered one.
	if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
	if (SUCCEEDED(hr) && on) hr = attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, on);
	if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(file.c_str(), nullptr, attributes.Get(), &writer);
	if (SUCCEEDED(hr)) hr = VideoType(hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264, width, height, &coded);
	if (SUCCEEDED(hr)) hr = coded->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(width * height * kFrameRate * (hevc ? kHevcBitsPerPixel : kBitsPerPixel)));
	if (SUCCEEDED(hr) && !hevc) hr = coded->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
	if (SUCCEEDED(hr)) hr = writer->AddStream(coded.Get(), &videoStream);
	if (SUCCEEDED(hr)) hr = VideoType(MFVideoFormat_NV12, width, height, &nv12);
	if (SUCCEEDED(hr)) hr = nv12->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
	if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(videoStream, nv12.Get(), nullptr);
	if (SUCCEEDED(hr) && capture) {
		hr = AudioType(MFAudioFormat_AAC, kAacBytesPerSecond, &aac);
		if (SUCCEEDED(hr)) hr = writer->AddStream(aac.Get(), &audioStream);
		if (SUCCEEDED(hr)) hr = AudioType(MFAudioFormat_PCM, kSampleRate * kAudioFrameBytes, &pcm);
		if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(audioStream, pcm.Get(), nullptr);
	}
	if (SUCCEEDED(hr)) hr = writer->BeginWriting();
	return hr;
}

// Which encoder the sink writer took, for the log: the export's name promises the graphics card's.
void Encoder::Impl::LogEncoder() {
	ComPtr<IMFTransform> encoder; ComPtr<IMFAttributes> attributes;
	wchar_t name[128] = L"unnamed"; UINT32 async = 0;
	if (FAILED(writer->GetServiceForStream(videoStream, GUID_NULL, IID_PPV_ARGS(&encoder))) || FAILED(encoder->GetAttributes(&attributes))) return;
	attributes->GetString(MFT_FRIENDLY_NAME_Attribute, name, 128, nullptr);
	// A driver's encoder may give no name; its vendor ("VEN_8086" is Intel) tells whose it is.
	if (!wcscmp(name, L"unnamed")) attributes->GetString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, name, 128, nullptr);
	attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async);
	const std::string narrow = WideToUtf8(name);
	summary = fmt::format("{}x{} {} by '{}' ({}{}), holding up to {} pictures{}", width, height, width > 4096 || height > 2160 ? "HEVC" : "H.264", narrow, async ? "hardware" : "software",
		card ? " on " + cardName : std::string(", Windows' choice"), holdLimit, capture ? "" : ", no sound");
	spdlog::info("Video: {}", summary);
}

Encoder::Encoder() : impl_(new Impl) {}

Encoder::~Encoder() { End(); }

const std::string& Encoder::Summary() const { return impl_->summary; }

bool Encoder::Begin(const std::wstring& file, unsigned width, unsigned height, unsigned long soundPid) {
	Impl& e = *impl_;
	// Delay-loaded (CMakeLists.txt): N editions without the Media Feature Pack have neither.
	if (e.writer || !width || !height || width % 2 || height % 2 || !LoadLibraryW(L"mfplat.dll") || !LoadLibraryW(L"mfreadwrite.dll")) return false;
	HRESULT hr = MFStartup(MF_VERSION);
	if (FAILED(hr)) { spdlog::warn("Video: Media Foundation did not start ({:#x})", static_cast<unsigned>(hr)); return false; }
	e.width = width; e.height = height; e.lastFrame = -1; e.pictures = e.dropped = 0; e.failed = false;
	e.summary.clear();
	// Six tenths of the address space still free, in pictures; 120 is two seconds, more than any queue needs.
	MEMORYSTATUSEX memory = {sizeof memory};
	GlobalMemoryStatusEx(&memory);
	e.holdLimit = static_cast<long>((std::min)(120ull, memory.ullAvailVirtual * 6 / 10 / (width * height * 3ull / 2)));
	e.OpenSound(soundPid);
	// On the preferred card first; where that card has no encoder for this
	// picture, the encoder Windows picks by itself, as before.
	e.card = PreferredCard(e.cardName);
	hr = e.card ? e.Open(file, e.card.Get()) : E_NOINTERFACE;
	if (FAILED(hr)) {
		if (e.card) spdlog::info("Video: no encoder on {} ({:#x}); taking the one Windows picks", e.cardName, static_cast<unsigned>(hr));
		e.writer.Reset(); e.card.Reset(); e.cardName.clear();
		DeleteFileW(file.c_str());
		hr = e.Open(file, nullptr);
	}
	e.returned = Microsoft::WRL::Make<Returned>();
	if (FAILED(hr)) {
		e.summary = fmt::format("the encoder did not open ({:#x})", static_cast<unsigned>(hr));
		spdlog::warn("Video: {}", e.summary);
		// The caller removes the file it named (VideoLinkServe.cxx).
		e.writer.Reset(); e.card.Reset(); e.capture.Reset(); e.client.Reset(); MFShutdown();
		return false;
	}
	e.LogEncoder();
	e.start = Clock();
	if (e.capture) {
		if (SUCCEEDED(e.client->Start())) { e.audioRuns = true; e.audio = std::thread(&Impl::CaptureSound, &e); }
		else e.failed = true;
	}
	return true;
}

void Encoder::Frame(const void* luma, int lumaPitch, const void* chroma, int chromaPitch, long long at) {
	Impl& e = *impl_;
	if (!e.writer || e.failed) return;
	ComPtr<IMFTrackedSample> tracked; ComPtr<IMFSample> sample; ComPtr<IMFMediaBuffer> buffer; BYTE* bytes = nullptr;
	const DWORD size = e.width * e.height * 3 / 2;
	if (e.returned->held >= e.holdLimit || FAILED(MFCreateMemoryBuffer(size, &buffer)) || FAILED(MFCreateTrackedSample(&tracked)) || FAILED(tracked.As(&sample)) ||
		FAILED(buffer->Lock(&bytes, nullptr, nullptr))) { e.dropped++; return; }
	MFCopyImage(bytes, e.width, static_cast<const BYTE*>(luma), lumaPitch, e.width, e.height);
	MFCopyImage(bytes + e.width * e.height, e.width, static_cast<const BYTE*>(chroma), chromaPitch, e.width, e.height / 2);
	buffer->Unlock(); buffer->SetCurrentLength(size);
	sample->AddBuffer(buffer.Get());
	// From here the sample's last release is counted (Returned).
	e.returned->held++;
	tracked->SetAllocator(e.returned.Get(), nullptr);
	// The real time it was drawn at, so the picture stays with the sound when a frame is missing.
	at = (std::max)(at - e.start, e.lastFrame + 1);
	e.lastFrame = at;
	sample->SetSampleTime(at);
	sample->SetSampleDuration(10000000 / kFrameRate);
	const HRESULT hr = e.writer->WriteSample(e.videoStream, sample.Get());
	if (SUCCEEDED(hr)) e.pictures++;
	else if (!e.failed) { e.failed = true; spdlog::warn("Video: the encoder stopped taking pictures after {} ({:#x})", e.pictures, static_cast<unsigned>(hr)); }
}

bool Encoder::End() {
	Impl& e = *impl_;
	if (!e.writer) return false;
	if (e.audioRuns.exchange(false)) { e.audio.join(); e.client->Stop(); }
	const HRESULT hr = e.writer->Finalize();
	e.summary = fmt::format("{} pictures written, {} dropped with the encoder behind, closed with {:#x}", e.pictures, e.dropped, static_cast<unsigned>(hr));
	spdlog::info("Video: {}", e.summary);
	e.writer.Reset(); e.card.Reset(); e.capture.Reset(); e.client.Reset();
	MFShutdown();
	return SUCCEEDED(hr) && e.pictures > 0 && !e.failed;
}

} } }
