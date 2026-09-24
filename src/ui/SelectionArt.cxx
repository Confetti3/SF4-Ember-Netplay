#include "SelectionArt.hxx"
#include "../common/FighterCatalog.hxx"
#include "../common/StageCatalog.hxx"
#include <windows.h>
#include <d3d9.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <zlib.h>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sf4e { namespace ui {
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::size_t MaximumFileBytes = 16 * 1024 * 1024;
constexpr std::size_t MaximumTextureBytes = 32 * 1024 * 1024;
constexpr UINT MaximumImageSide = 512;
// A file that exists but cannot be read, decoded or uploaded is tried again
// after 1, then 2 seconds (at 60 fps) before it is reported missing, so a
// transient failure (memory pressure, a device mid-reset) does not blank the
// art for the rest of the session. An image with no file at all is final.
constexpr int MaximumAttempts = 3;
constexpr std::uint64_t RetryFrames = 60;
std::function<void(const std::string&)>& Logger() {
    static std::function<void(const std::string&)> logger;
    return logger;
}
std::string Narrow(const std::wstring& text) {
    std::string result;
    for (const wchar_t c : text) result += c < 0x80 ? static_cast<char>(c) : '?';
    return result;
}
std::string Hex(HRESULT value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(value));
    return text;
}

std::uint32_t U32(const std::vector<unsigned char>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    if (offset + sizeof(value) <= bytes.size()) std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
// `opened` tells a missing file apart from one that exists but is unusable.
std::vector<unsigned char> ReadImage(const std::wstring& path, bool& opened) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    opened = static_cast<bool>(file);
    if (!file) return {};
    const auto length = file.tellg();
    if (length <= 0 || length > static_cast<std::streamoff>(MaximumFileBytes)) return {};
    std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
    file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), length);
    if (!file) return {};
    if (bytes.size() >= 16 && std::memcmp(bytes.data(), "#EMZ", 4) == 0) {
        const auto size = U32(bytes, 8), start = U32(bytes, 12);
        if (!size || size > MaximumFileBytes || start < 16 || start >= bytes.size()) return {};
        std::vector<unsigned char> expanded(size);
        z_stream stream{};
        stream.next_in = bytes.data() + start; stream.avail_in = static_cast<uInt>(bytes.size() - start);
        stream.next_out = expanded.data(); stream.avail_out = size;
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return {};
        const int result = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        if (result != Z_STREAM_END || stream.total_out != size) return {};
        bytes.swap(expanded);
        if (bytes.size() < 40 || std::memcmp(bytes.data(), "#EMB", 4) != 0) return {};
        // Asymmetric fighters also have sel_chara_rev.dds for the other side.
        // The first entry is the standard portrait in both archive layouts.
        const auto count = U32(bytes, 12);
        if (count < 1 || count > 2) return {};
        const std::size_t table = U32(bytes, 24);
        if (table + count * 8 > bytes.size()) return {};
        const std::size_t image = table + U32(bytes, table), imageBytes = U32(bytes, table + 4);
        if (image > bytes.size() || imageBytes > bytes.size() - image || imageBytes < 128 ||
            std::memcmp(bytes.data() + image, "DDS ", 4) != 0) return {};
        return std::vector<unsigned char>(bytes.begin() + image, bytes.begin() + image + imageBytes);
    }
    return bytes;
}
struct Pixels {
    std::string key;
    UINT width = 0, height = 0;
    std::vector<unsigned char> bgra;
    bool found = false;   // some candidate file existed
    std::string failure;  // why the last existing candidate was unusable
};
Pixels Decode(IWICImagingFactory* factory, const std::string& key, const std::vector<std::wstring>& paths, UINT side) {
    Pixels pixels; pixels.key = key;
    for (const auto& path : paths) {
        bool opened = false;
        auto bytes = ReadImage(path, opened);
        pixels.found = pixels.found || opened;
        if (bytes.empty()) {
            if (opened) pixels.failure = Narrow(path) + ": unreadable or unsupported container";
            continue;
        }
        HRESULT hr = S_OK;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICFormatConverter> converter;
        UINT width = 0, height = 0;
        if (FAILED(hr = factory->CreateStream(&stream)) ||
            FAILED(hr = stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size()))) ||
            FAILED(hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
            FAILED(hr = decoder->GetFrame(0, &frame)) || FAILED(hr = frame->GetSize(&width, &height)) ||
            !width || !height || width > 8192 || height > 8192) {
            pixels.failure = Narrow(path) + ": decode " + Hex(hr) + " " + std::to_string(width) + "x" + std::to_string(height);
            continue;
        }
        const double ratio = (std::min)(1.0, double(side) / (std::max)(width, height));
        width = (std::max)(1u, static_cast<UINT>(width * ratio));
        height = (std::max)(1u, static_cast<UINT>(height * ratio));
        if (FAILED(hr = factory->CreateBitmapScaler(&scaler)) ||
            FAILED(hr = scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant)) ||
            FAILED(hr = factory->CreateFormatConverter(&converter)) ||
            FAILED(hr = converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            pixels.failure = Narrow(path) + ": convert " + Hex(hr);
            continue;
        }
        pixels.bgra.resize(width * height * 4);
        if (FAILED(hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.bgra.size()), pixels.bgra.data()))) {
            pixels.failure = Narrow(path) + ": copy " + Hex(hr);
            pixels.bgra.clear(); continue;
        }
        pixels.width = width; pixels.height = height;
        return pixels;
    }
    return pixels;
}
}

struct SelectionArt::Impl {
    struct Job { std::string key; std::vector<std::wstring> paths; UINT side; };
    struct Entry {
        ComPtr<IDirect3DTexture9> texture;
        UINT width = 0, height = 0;
        bool complete = false;
        bool queued = false;
        int failures = 0;
        std::uint64_t retryFrame = 0;
        ImVec2 uvMin = ImVec2(0, 0), uvMax = ImVec2(1, 1);
        std::uint64_t lastUse = 0;
    };
    ComPtr<IDirect3DDevice9> device;
    std::wstring gameRoot, assetRoot;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::deque<Pixels> completed;
    std::map<std::string, Entry> entries;
    bool stop = false;
    std::thread worker;
    std::uint64_t frame = 0;
    std::size_t textureBytes = 0;

    Impl(IDirect3DDevice9* d, std::wstring game, std::wstring assets)
        : device(d), gameRoot(std::move(game)), assetRoot(std::move(assets)), worker([this] { Work(); }) {}
    ~Impl() {
        { std::lock_guard<std::mutex> lock(mutex); stop = true; }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }
    void Work() {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            ComPtr<IWICImagingFactory> factory;
            const bool haveFactory = SUCCEEDED(initialized) && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,
                nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
            for (;;) {
                Job job;
                { std::unique_lock<std::mutex> lock(mutex);
                  wake.wait(lock, [&] { return stop || (!jobs.empty() && completed.size() < 4); });
                  if (stop) break;
                  job = std::move(jobs.front()); jobs.pop_front(); }
                Pixels pixels; pixels.key = job.key;
                try {
                    if (haveFactory) pixels = Decode(factory.Get(), job.key, job.paths, job.side);
                    else { pixels.found = true; pixels.failure = "image decoder unavailable"; }
                } catch (const std::exception& error) {
                    pixels.bgra.clear(); pixels.found = true; pixels.failure = std::string("decode threw: ") + error.what();
                }
                { std::lock_guard<std::mutex> lock(mutex); completed.push_back(std::move(pixels)); }
            }
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
    }
    SelectionImage Request(const std::string& key, std::vector<std::wstring> paths, UINT side = 256) {
        auto found = entries.find(key);
        const bool retry = found != entries.end() && !found->second.complete && !found->second.queued &&
            frame >= found->second.retryFrame;
        if (found == entries.end() || retry) {
            std::lock_guard<std::mutex> lock(mutex);
            if (jobs.size() >= 128) return {};
            if (found == entries.end()) found = entries.emplace(key, Entry{}).first;
            found->second.queued = true;
            jobs.push_back({key, std::move(paths), side}); wake.notify_one();
        }
        auto& entry = found->second; entry.lastUse = frame;
        SelectionImage result;
        result.texture = reinterpret_cast<ImTextureID>(entry.texture.Get());
        result.uvMin = entry.uvMin; result.uvMax = entry.uvMax;
        result.width = static_cast<int>(entry.width * (entry.uvMax.x - entry.uvMin.x));
        result.height = static_cast<int>(entry.height * (entry.uvMax.y - entry.uvMin.y));
        result.missing = entry.complete && !entry.texture;
        return result;
    }
    // An image with no file anywhere is final and expected for most color
    // previews. One that exists but failed is retried, then logged once.
    void Failed(Entry& entry, const std::string& key, bool found, const std::string& failure) {
        if (found && ++entry.failures < MaximumAttempts) {
            entry.retryFrame = frame + RetryFrames * entry.failures;
            return;
        }
        entry.complete = true;
        const bool portrait = key.find("/portrait-") != std::string::npos;
        if (!Logger()) return;
        if (found) Logger()("Selection art " + key + " unavailable after " + std::to_string(entry.failures) + " attempts: " + failure);
        else if (portrait) Logger()("Selection art " + key + " unavailable: no game or package file");
    }
    std::vector<std::wstring> ImagePaths(const std::string& key) const {
        const std::wstring relative(key.begin(), key.end());
        return {assetRoot + L"/" + relative + L"-cutout.png", assetRoot + L"/" + relative + L".png", assetRoot + L"/" + relative + L".jpg"};
    }
};

void SelectionArt::SetLogger(std::function<void(const std::string&)> logger) { Logger() = std::move(logger); }
SelectionArt::SelectionArt(IDirect3DDevice9* device, std::wstring game, std::wstring assets)
    : impl_(new Impl(device, std::move(game), std::move(assets))) {}
SelectionArt::~SelectionArt() = default;
SelectionImage SelectionArt::MenuBackdrop() {
    return impl_->Request("ember-menu-background",{impl_->assetRoot+L"/../brand/ember-menu-background.png"},2048);
}
SelectionImage SelectionArt::InputPrompt(const std::string& name) {
    return impl_->Request("input-"+name,{impl_->assetRoot+L"/../input/"+std::wstring(name.begin(),name.end())+L".png"},128);
}
void SelectionArt::Pump() {
    auto& state = *impl_; ++state.frame;
    for (int upload = 0; upload < 2; ++upload) {
        Pixels pixels;
        { std::lock_guard<std::mutex> lock(state.mutex);
          if (state.completed.empty()) break;
          pixels = std::move(state.completed.front()); state.completed.pop_front(); }
        state.wake.notify_one();
        auto& entry = state.entries[pixels.key]; entry.queued = false;
        if (pixels.bgra.empty()) { state.Failed(entry, pixels.key, pixels.found, pixels.failure); continue; }
        ComPtr<IDirect3DTexture9> texture;
        HRESULT hr = state.device->CreateTexture(pixels.width, pixels.height, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, &texture, nullptr);
        if (FAILED(hr)) { state.Failed(entry, pixels.key, true, "CreateTexture " + Hex(hr)); continue; }
        D3DLOCKED_RECT locked{};
        if (FAILED(hr = texture->LockRect(0, &locked, nullptr, 0))) { state.Failed(entry, pixels.key, true, "LockRect " + Hex(hr)); continue; }
        entry.complete = true;
        for (UINT row = 0; row < pixels.height; ++row)
            std::memcpy(static_cast<unsigned char*>(locked.pBits) + row * locked.Pitch,
                        pixels.bgra.data() + row * pixels.width * 4, pixels.width * 4);
        texture->UnlockRect(0);
        entry.texture = std::move(texture); entry.width = pixels.width; entry.height = pixels.height;
        // Fit the painted portrait, rather than its large transparent canvas.
        // The source pixels stay unchanged; only the UI's texture coordinates differ.
        if (pixels.key.find("/portrait-") != std::string::npos) {
            UINT left = pixels.width, top = pixels.height, right = 0, bottom = 0;
            for (UINT y = 0; y < pixels.height; ++y) for (UINT x = 0; x < pixels.width; ++x) {
                if (pixels.bgra[(y * pixels.width + x) * 4 + 3] < 8) continue;
                left = (std::min)(left, x); top = (std::min)(top, y);
                right = (std::max)(right, x + 1); bottom = (std::max)(bottom, y + 1);
            }
            if (left < right && top < bottom) {
                entry.uvMin = ImVec2(float(left > 1 ? left - 2 : 0) / pixels.width, float(top > 1 ? top - 2 : 0) / pixels.height);
                entry.uvMax = ImVec2(float((std::min)(right + 2, pixels.width)) / pixels.width,
                                    float((std::min)(bottom + 2, pixels.height)) / pixels.height);
            }
        }
        state.textureBytes += pixels.bgra.size();
    }
    while (state.textureBytes > MaximumTextureBytes) {
        auto oldest = state.entries.end();
        for (auto it = state.entries.begin(); it != state.entries.end(); ++it)
            if (it->second.texture && it->second.lastUse + 2 < state.frame &&
                (oldest == state.entries.end() || it->second.lastUse < oldest->second.lastUse)) oldest = it;
        if (oldest == state.entries.end()) break;
        state.textureBytes -= oldest->second.width * oldest->second.height * 4;
        state.entries.erase(oldest);
    }
}
SelectionImage SelectionArt::Portrait(int fighterId, bool large) {
    const auto* fighter = selection::FindFighter(fighterId);
    if (!fighter) return {};
    const std::string key = std::string(fighter->code) + "/portrait";
    auto paths = impl_->ImagePaths(key);
    const std::wstring code(fighter->code, fighter->code + 3);
    for (const wchar_t* root : {L"patch_ae2_tu3", L"patch_ae2_tu2", L"patch_ae2", L"dlc/04_ae2",
                               L"dlc/03_character_free", L"resource"})
        paths.push_back(impl_->gameRoot + L"/" + root + L"/ui/chara_select/chara/sel_" + code + L".tex.emz");
    // Last resort when the game's portrait cannot be read or decoded: the
    // packaged original-outfit cutout, cropped the same way.
    const auto outfit = impl_->ImagePaths(std::string(fighter->code) + "/costume-0/color-0");
    paths.insert(paths.end(), outfit.begin(), outfit.end());
    return impl_->Request(key + (large ? "-large" : "-thumb"), std::move(paths), large ? MaximumImageSide : 128);
}
SelectionImage SelectionArt::Appearance(int fighterId, int costume, int color) {
    const auto* fighter = selection::FindFighter(fighterId);
    if (!fighter || color < 0 || color >= selection::ColorCount(fighterId, costume)) return {};
    const std::string key = std::string(fighter->code) + "/costume-" + std::to_string(costume) + "/color-" + std::to_string(color);
    const auto result = impl_->Request(key, impl_->ImagePaths(key));
    // Native selection art depicts the original outfit in its default palette.
    if (result.missing && costume == 0 && color == 0) return Portrait(fighterId, true);
    return result;
}
SelectionImage SelectionArt::Ultra(int fighterId, int ultra) {
    const auto* fighter = selection::FindFighter(fighterId);
    if (!fighter || ultra < 0 || ultra > 1) return {};
    const std::string key = std::string(fighter->code) + "/ultra-" + std::to_string(ultra);
    return impl_->Request(key, impl_->ImagePaths(key));
}
SelectionImage SelectionArt::Stage(int nativeId) {
    const auto* stage = selection::FindStage(nativeId);
    if (!stage) return {};
    const std::string key = std::string("stages/") + stage->code;
    return impl_->Request(key, impl_->ImagePaths(key), MaximumImageSide);
}
} }
