#include "MovieFrameService.hxx"
#include "MovieFrameMailbox.hxx"
#include "../common/MovieFrames.hxx"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

namespace sf4e { namespace platform { namespace movie_frames {

using Microsoft::WRL::ComPtr;
namespace frames = sf4e::movie_frames;

namespace {

std::string Hex(HRESULT value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(value));
    return text;
}

// What says no frame of any movie can be decoded here, rather than that this
// one could not: no source for the container, no decoder for WMV3, no
// conversion to RGB32. Every listed movie is the same container and codec.
bool NothingDecodes(HRESULT hr) {
    return hr == MF_E_TOPO_CODEC_NOT_FOUND || hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE || hr == MF_E_UNSUPPORTED_SCHEME ||
        hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_UNSUPPORTED_FORMAT || hr == REGDB_E_CLASSNOTREG || hr == E_NOTIMPL ||
        hr == E_NOINTERFACE || hr == CO_E_NOTINITIALIZED;
}

// The frame's pixels, top-down BGRA (alpha set), from a sample of the
// reader's RGB32 output.
bool CopyFrame(IMFSample* sample, UINT32 width, UINT32 height, LONG defaultStride, std::vector<BYTE>& bgra, std::string& why) {
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
    if (FAILED(hr)) return why = "ConvertToContiguousBuffer " + Hex(hr), false;
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    ComPtr<IMF2DBuffer> buffer2d;
    const bool twoD = SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch));
    if (!twoD) {
        BYTE* data = nullptr;
        if (FAILED(hr = buffer->Lock(&data, nullptr, &length))) return why = "Lock " + Hex(hr), false;
        pitch = defaultStride ? defaultStride : static_cast<LONG>(width * 4);
        const std::size_t span = static_cast<std::size_t>(pitch < 0 ? -pitch : pitch);
        if (span < width * 4 || length < span * height) {
            buffer->Unlock();
            return why = "a sample smaller than its frame", false;
        }
        // A negative stride is a bottom-up picture: its first row is the last in memory.
        scan0 = pitch < 0 ? data + span * (height - 1) : data;
    }
    bgra.resize(static_cast<std::size_t>(width) * height * 4);
    for (UINT32 y = 0; y < height; ++y) {
        const BYTE* row = scan0 + static_cast<std::ptrdiff_t>(pitch) * static_cast<std::ptrdiff_t>(y);
        BYTE* out = bgra.data() + static_cast<std::size_t>(y) * width * 4;
        std::memcpy(out, row, static_cast<std::size_t>(width) * 4);
        for (UINT32 x = 0; x < width; ++x) out[x * 4 + 3] = 255; // RGB32's fourth byte is undefined
    }
    if (twoD) buffer2d->Unlock2D();
    else buffer->Unlock();
    return true;
}

// `bgra` (width by height) fitted within the most size and written to `file`
// as a JPEG, through a temporary file beside it.
bool WriteJpeg(const std::vector<BYTE>& bgra, UINT width, UINT height, const std::wstring& file, std::string& why) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return why = "WIC " + Hex(hr), false;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(hr = factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, width * 4,
            static_cast<UINT>(bgra.size()), const_cast<BYTE*>(bgra.data()), &bitmap)))
        return why = "CreateBitmapFromMemory " + Hex(hr), false;
    int fittedWidth = 0, fittedHeight = 0;
    frames::FitWithin(static_cast<int>(width), static_cast<int>(height), fittedWidth, fittedHeight);
    ComPtr<IWICBitmapSource> source = bitmap;
    ComPtr<IWICBitmapScaler> scaler;
    if (static_cast<UINT>(fittedWidth) != width || static_cast<UINT>(fittedHeight) != height) {
        if (FAILED(hr = factory->CreateBitmapScaler(&scaler)) ||
            FAILED(hr = scaler->Initialize(bitmap.Get(), fittedWidth, fittedHeight, WICBitmapInterpolationModeFant)))
            return why = "scale " + Hex(hr), false;
        source = scaler;
    }
    // JPEG holds no alpha: 24-bit BGR.
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(hr = factory->CreateFormatConverter(&converter)) ||
        FAILED(hr = converter->Initialize(source.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
            WICBitmapPaletteTypeCustom)))
        return why = "convert " + Hex(hr), false;
    const std::wstring temporary = file + L".part" + std::to_wstring(GetCurrentProcessId());
    {
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        if (FAILED(hr = factory->CreateStream(&stream)) || FAILED(hr = stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE)))
            return why = "create the file " + Hex(hr), false;
        if (FAILED(hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) ||
            FAILED(hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
            FAILED(hr = encoder->CreateNewFrame(&frame, &options))) {
            stream.Reset();
            DeleteFileW(temporary.c_str());
            return why = "JPEG encoder " + Hex(hr), false;
        }
        PROPBAG2 quality = {};
        quality.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = 0.9f;
        options->Write(1, &quality, &value); // the encoder's default quality otherwise
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (FAILED(hr = frame->Initialize(options.Get())) || FAILED(hr = frame->SetSize(fittedWidth, fittedHeight)) ||
            FAILED(hr = frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat24bppBGR ||
            FAILED(hr = frame->WriteSource(converter.Get(), nullptr)) || FAILED(hr = frame->Commit()) || FAILED(hr = encoder->Commit())) {
            frame.Reset(); encoder.Reset(); stream.Reset();
            DeleteFileW(temporary.c_str());
            return why = "JPEG " + Hex(hr), false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        DeleteFileW(temporary.c_str());
        return why = "rename " + std::to_string(error), false;
    }
    return true;
}

} // namespace

Decoded DecodeFrame(const std::wstring& movie, std::uint32_t ms, const std::wstring& file, const std::atomic<bool>* cancel, std::string& why) {
    if (GetFileAttributesW(movie.c_str()) == INVALID_FILE_ATTRIBUTES) return why = "the movie is not in the game folder", Decoded::Failed;
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateAttributes(&attributes, 2);
    // The reader's own converter turns the decoder's YUV into RGB32. No
    // Direct3D device: decoded on the processor, never on the graphics card
    // the game draws with.
    if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(movie.c_str(), attributes.Get(), &reader);
    if (FAILED(hr)) return why = "no source for the movie " + Hex(hr), NothingDecodes(hr) ? Decoded::Unavailable : Decoded::Failed;
    constexpr DWORD video = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (FAILED(hr = reader->SetStreamSelection(video, TRUE))) return why = "no picture in the movie " + Hex(hr), Decoded::Failed;
    ComPtr<IMFMediaType> wanted, current;
    hr = MFCreateMediaType(&wanted);
    if (SUCCEEDED(hr)) hr = wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(video, nullptr, wanted.Get());
    if (FAILED(hr)) return why = "no decoder to RGB32 " + Hex(hr), NothingDecodes(hr) ? Decoded::Unavailable : Decoded::Failed;
    UINT32 width = 0, height = 0;
    LONG stride = 0;
    const auto readType = [&]() -> HRESULT {
        current.Reset();
        HRESULT read = reader->GetCurrentMediaType(video, &current);
        if (SUCCEEDED(read)) read = MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width, &height);
        if (FAILED(read)) return read;
        stride = static_cast<LONG>(MFGetAttributeUINT32(current.Get(), MF_MT_DEFAULT_STRIDE, 0));
        if (!stride && FAILED(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &stride))) stride = 0;
        return width && height && width <= 8192 && height <= 8192 ? S_OK : E_UNEXPECTED;
    };
    if (FAILED(hr = readType())) return why = "no frame size " + Hex(hr), Decoded::Failed;
    // The position is in 100 ns units; the reader goes to the key frame before
    // it, and the samples are read on to the wanted time.
    const LONGLONG target = static_cast<LONGLONG>(ms) * 10000;
    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = target;
    if (FAILED(hr = reader->SetCurrentPosition(GUID_NULL, position))) return why = "seek " + Hex(hr), Decoded::Failed;
    ComPtr<IMFSample> kept;
    // At most a minute of 24 fps samples past the key frame: a movie that
    // never reaches the time gives the last frame it has.
    for (int read = 0; read < 1440; ++read) {
        if (cancel && cancel->load()) return why = "stopped", Decoded::Failed;
        DWORD flags = 0;
        LONGLONG at = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(hr = reader->ReadSample(video, 0, nullptr, &flags, &at, &sample))) return why = "ReadSample " + Hex(hr), Decoded::Failed;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            if (FAILED(hr = readType())) return why = "frame size changed " + Hex(hr), Decoded::Failed;
            kept.Reset(); // a sample of the old size
        }
        if (sample) kept = sample;
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) break;
        // Within half a 24 fps frame of the time counts as it.
        if (sample && at + 208333 >= target) break;
    }
    if (!kept) return why = "no frame at that time", Decoded::Failed;
    std::vector<BYTE> bgra;
    if (!CopyFrame(kept.Get(), width, height, stride, bgra, why)) return Decoded::Failed;
    kept.Reset(); reader.Reset();
    return WriteJpeg(bgra, width, height, file, why) ? Decoded::Ok : Decoded::Failed;
}

// What the serving thread uses, shared with it: a thread that does not end
// in time when the service stops (a decoder stuck inside Media Foundation,
// as can happen under Wine) is left to the process's exit, and must not
// reach into a service that is gone.
struct MovieFrameService::Shared {
    std::wstring gameFolder, cache;
    Logger log;
    HANDLE stop = nullptr;
    std::atomic<bool> cancel{false};
    MovieFrameHost host;
    ~Shared() {
        if (stop) CloseHandle(stop);
    }
    void Log(const std::string& line) const {
        if (log) log(line);
    }
};

namespace {

// The serving thread: one request at a time, until `shared.stop`.
void Serve(MovieFrameService::Shared& shared) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool started = false, unavailable = false, folderMade = false;
    for (;;) {
        const HANDLE waits[] = {shared.stop, shared.host.ReadyEvent()};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) break;
        std::uint32_t request = 0, ms = 0;
        std::string movie;
        if (!shared.host.Take(request, movie, ms)) continue;
        const int index = frames::Find(movie, ms);
        if (index < 0) {
            shared.host.Reply(request, Answer::Failed);
            shared.Log("Movie frames: refused a request for a frame not on the list");
            continue;
        }
        const std::string name = frames::FileName(frames::Curated()[static_cast<std::size_t>(index)]);
        if (!unavailable && !folderMade) {
            std::error_code error;
            if (!shared.cache.empty()) std::filesystem::create_directories(shared.cache, error);
            if (shared.cache.empty() || error) {
                unavailable = true;
                shared.Log("Movie frames: no cache folder (" + (shared.cache.empty() ? std::string("Local AppData unknown") : error.message()) +
                    "); none are decoded");
            } else folderMade = true;
        }
        const std::wstring file = shared.cache + L"\\" + std::wstring(name.begin(), name.end());
        if (!unavailable && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) {
            shared.host.Reply(request, Answer::Ready);
            continue;
        }
        if (!unavailable && !started) {
            // Delay-loaded: present on Windows but N editions without the
            // Media Feature Pack, and not always under Wine.
            if (!LoadLibraryW(L"mfplat.dll") || !LoadLibraryW(L"mfreadwrite.dll")) {
                unavailable = true;
                shared.Log("Movie frames: Media Foundation is not installed; none are decoded");
            } else if (const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET); FAILED(hr)) {
                unavailable = true;
                shared.Log("Movie frames: Media Foundation did not start (" + Hex(hr) + "); none are decoded");
            } else started = true;
        }
        if (unavailable || FAILED(com)) {
            shared.host.Reply(request, Answer::Unavailable);
            continue;
        }
        std::wstring path = shared.gameFolder + L"\\" + std::wstring(movie.begin(), movie.end());
        for (auto& c : path)
            if (c == L'/') c = L'\';
        const auto began = std::chrono::steady_clock::now();
        std::string why;
        const Decoded decoded = DecodeFrame(path, ms, file, &shared.cancel, why);
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
        if (decoded == Decoded::Unavailable) unavailable = true;
        shared.host.Reply(request, decoded == Decoded::Ok ? Answer::Ready : decoded == Decoded::Unavailable ? Answer::Unavailable : Answer::Failed);
        shared.Log("Movie frames: " + movie + " at " + std::to_string(ms) + " ms " +
            (decoded == Decoded::Ok ? std::string("decoded") : decoded == Decoded::Unavailable ? "cannot be decoded here, nor any other: " + why : "failed: " + why) +
            " (" + std::to_string(took) + " ms)");
    }
    if (started) MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
}

} // namespace

bool MovieFrameService::Start(DWORD gamePid, std::wstring gameFolder, Logger log, std::wstring mailbox, std::wstring cache) {
    Stop();
    auto shared = std::make_shared<Shared>();
    shared->gameFolder = std::move(gameFolder);
    shared->cache = cache.empty() ? CacheFolder() : std::move(cache);
    shared->log = std::move(log);
    shared->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // The mailbox is made here, before the thread, so the game finds it from
    // its first frame; the thread only serves it.
    if (!shared->stop || shared->gameFolder.empty() || !shared->host.Create(mailbox.empty() ? MailboxName(gamePid) : mailbox)) return false;
    shared_ = shared;
    thread_ = std::thread([shared] { Serve(*shared); });
    return true;
}

void MovieFrameService::Stop() {
    if (shared_) {
        shared_->cancel = true;
        SetEvent(shared_->stop);
    }
    if (thread_.joinable()) {
        // A decode stops at its next sample; one stuck inside Media
        // Foundation is left to the process's exit rather than holding the
        // launcher open after the game.
        if (WaitForSingleObject(static_cast<HANDLE>(thread_.native_handle()), 5000) == WAIT_OBJECT_0) thread_.join();
        else {
            if (shared_) shared_->Log("Movie frames: the decoder did not stop in 5 s; left to end with the launcher");
            thread_.detach();
        }
    }
    shared_.reset();
}

} } }
