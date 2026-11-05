#include "SelectionArt.hxx"
#include "../common/ProductIdentity.hxx"
#include "../platform/Utf8.hxx"
#include <windows.h>
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
// A file that exists but cannot be read, decoded or uploaded is tried again
// after 1, then 2 seconds (at 60 fps) before it is reported missing, so a
// transient failure (memory pressure, a device mid-reset) does not blank the
// art for the rest of the session. An image with no file at all is final.
constexpr int MaximumAttempts = 3;
constexpr std::uint64_t RetryFrames = 60;
// A portrait archive holds the portrait and, for an asymmetric fighter, its
// mirror; an archive read by entry name can hold the whole of a game screen.
constexpr std::size_t MaximumPortraitEntries = 2, MaximumArchiveEntries = 1024;
using platform::WideToUtf8;
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
// The image of an EMB archive: the entry named `entry`, or with none the first
// (the standard portrait in both portrait archive layouts, which hold one or two).
std::vector<unsigned char> EmbImage(const std::vector<unsigned char>& emb, const std::string& entry) {
    if (emb.size() < 40 || std::memcmp(emb.data(), "#EMB", 4) != 0) return {};
    const std::size_t count = U32(emb, 12), table = U32(emb, 24), names = U32(emb, 28);
    if (count < 1 || count > (entry.empty() ? MaximumPortraitEntries : MaximumArchiveEntries) || table + count * 8 > emb.size()) return {};
    std::size_t index = 0;
    if (!entry.empty()) {
        if (!names || names + count * 4 > emb.size()) return {};
        const auto named = [&](std::size_t i) {
            const std::size_t name = U32(emb, names + i * 4);
            return name < emb.size() && emb.size() - name > entry.size() &&
                std::memcmp(emb.data() + name, entry.data(), entry.size()) == 0 && emb[name + entry.size()] == 0;
        };
        while (index < count && !named(index)) ++index;
        if (index == count) return {};
    }
    // Each entry's offset counts from its own row of the table.
    const std::size_t row = table + index * 8;
    const std::size_t image = row + U32(emb, row), imageBytes = U32(emb, row + 4);
    if (image > emb.size() || imageBytes > emb.size() - image || imageBytes < 128 ||
        std::memcmp(emb.data() + image, "DDS ", 4) != 0) return {};
    return std::vector<unsigned char>(emb.begin() + image, emb.begin() + image + imageBytes);
}
// `opened` tells a missing file apart from one that exists but is unusable. An
// EMZ archive yields its image named `entry` (EmbImage).
std::vector<unsigned char> ReadImage(const std::wstring& path, bool& opened, const std::string& entry) {
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
        // Asymmetric fighters also have sel_chara_rev.dds for the other side.
        return EmbImage(expanded, entry);
    }
    return bytes;
}
// Windows' DDS codec reads only block-compressed images. The game's class
// emblems (Ranking.emz) are plain 32-bit BGRA, so those pixels are taken as
// they are; anything else goes to the codec.
ComPtr<IWICBitmapSource> PlainDds(IWICImagingFactory* factory, const std::vector<unsigned char>& bytes) {
    constexpr std::size_t Header = 128;
    constexpr std::uint32_t Rgb = 0x40, AlphaPixels = 0x1;
    if (bytes.size() < Header || std::memcmp(bytes.data(), "DDS ", 4) != 0 || U32(bytes, 4) != 124) return nullptr;
    const std::uint32_t height = U32(bytes, 12), width = U32(bytes, 16), format = U32(bytes, 80);
    if ((format & (Rgb | AlphaPixels)) != (Rgb | AlphaPixels) || U32(bytes, 88) != 32 || U32(bytes, 92) != 0xff0000 ||
        U32(bytes, 96) != 0xff00 || U32(bytes, 100) != 0xff || U32(bytes, 104) != 0xff000000 ||
        !width || !height || width > 8192 || height > 8192 || (bytes.size() - Header) / 4 / width < height) return nullptr;
    // The bitmap keeps its own copy of the pixels.
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICBitmapSource> source;
    if (FAILED(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, width * 4, width * height * 4,
            const_cast<BYTE*>(bytes.data() + Header), &bitmap)) || FAILED(bitmap.As(&source))) return nullptr;
    return source;
}
// Absent: no candidate file exists, which is final and expected for most
// color previews. Failed: a file exists but could not be used; worth a retry.
enum class Outcome { Decoded, Absent, Failed };
struct Pixels {
    std::string key;
    UINT width = 0, height = 0;
    std::vector<unsigned char> bgra;
    Outcome outcome = Outcome::Absent;
    // The first existing candidate that was unusable. With Decoded, a later
    // candidate stood in for it.
    std::string failure;
    void Fail(const std::wstring& path, const std::string& reason) {
        outcome = Outcome::Failed;
        if (failure.empty()) failure = WideToUtf8(path) + ": " + reason;
    }
};
Pixels Decode(IWICImagingFactory* factory, const std::string& key, const std::vector<std::wstring>& paths, UINT side,
    const std::string& entry) {
    Pixels pixels; pixels.key = key;
    for (const auto& path : paths) {
        bool opened = false;
        auto bytes = ReadImage(path, opened, entry);
        if (bytes.empty()) {
            if (opened) pixels.Fail(path, "unreadable or unsupported container");
            continue;
        }
        HRESULT hr = S_OK;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICBitmapScaler> scaler;
        ComPtr<IWICFormatConverter> converter;
        UINT width = 0, height = 0;
        ComPtr<IWICBitmapSource> image = PlainDds(factory, bytes);
        if (!image && SUCCEEDED(hr = factory->CreateStream(&stream)) &&
            SUCCEEDED(hr = stream->InitializeFromMemory(bytes.data(), static_cast<DWORD>(bytes.size()))) &&
            SUCCEEDED(hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(hr = decoder->GetFrame(0, &frame))) {
            image = frame;
        }
        if (!image || FAILED(hr = image->GetSize(&width, &height)) || !width || !height || width > 8192 || height > 8192) {
            pixels.Fail(path, "decode " + Hex(hr) + " " + std::to_string(width) + "x" + std::to_string(height));
            continue;
        }
        // NativeSide: an atlas, whose cells are addressed in its own pixels, is
        // converted at its own size and never goes through the scaler.
        if (side != SelectionArt::NativeSide) {
            const double ratio = (std::min)(1.0, double(side) / (std::max)(width, height));
            width = (std::max)(1u, static_cast<UINT>(width * ratio));
            height = (std::max)(1u, static_cast<UINT>(height * ratio));
            if (FAILED(hr = factory->CreateBitmapScaler(&scaler)) ||
                FAILED(hr = scaler->Initialize(image.Get(), width, height, WICBitmapInterpolationModeFant))) {
                pixels.Fail(path, "scale " + Hex(hr));
                continue;
            }
        }
        IWICBitmapSource* source = side == SelectionArt::NativeSide ? image.Get() : scaler.Get();
        if (FAILED(hr = factory->CreateFormatConverter(&converter)) ||
            FAILED(hr = converter->Initialize(source, GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            pixels.Fail(path, "convert " + Hex(hr));
            continue;
        }
        pixels.bgra.resize(width * height * 4);
        if (FAILED(hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.bgra.size()), pixels.bgra.data()))) {
            pixels.Fail(path, "copy " + Hex(hr));
            pixels.bgra.clear(); continue;
        }
        pixels.width = width; pixels.height = height;
        pixels.outcome = Outcome::Decoded;
        return pixels;
    }
    return pixels;
}
}

struct SelectionArt::Impl {
    // `entry` names the image inside an archive candidate; empty takes its first.
    struct Job { std::string key; std::vector<std::wstring> paths; UINT side; std::string entry; };
    // Queued: a decode is in flight. Waiting: the last attempt failed and a
    // retry is due at retryFrame. Ready and Missing are final.
    enum class State { Queued, Waiting, Ready, Missing };
    // An uploaded image, released through the uploader when replaced or erased.
    struct Texture {
        TextureUploader* uploader = nullptr;
        ImTextureID id = 0;
        Texture() = default;
        Texture(TextureUploader* owner, ImTextureID made) : uploader(owner), id(made) {}
        Texture(Texture&& other) noexcept : uploader(other.uploader), id(other.id) { other.id = 0; }
        Texture& operator=(Texture&& other) noexcept {
            if (this != &other) { Reset(); uploader = other.uploader; id = other.id; other.id = 0; }
            return *this;
        }
        ~Texture() { Reset(); }
        void Reset() { if (id) uploader->Release(id); id = 0; }
        explicit operator bool() const { return id != 0; }
    };
    struct Entry {
        Policy policy;
        State state = State::Queued;
        Texture texture;
        UINT width = 0, height = 0;
        int failures = 0;
        std::uint64_t retryFrame = 0;
        ImVec2 uvMin = ImVec2(0, 0), uvMax = ImVec2(1, 1);
        std::uint64_t lastUse = 0;
    };
    // Declared before entries, so it outlives every texture it made.
    std::unique_ptr<TextureUploader> uploader;
    SelectionArt::Logger log;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::deque<Pixels> completed;
    std::map<std::string, Entry> entries;
    bool stop = false;
    std::thread worker;
    std::uint64_t frame = 0;
    std::size_t textureBytes = 0;
    // Set by Request; Pump does nothing on frames where no art was drawn, so
    // a closed menu costs a match nothing.
    bool requested = false;

    Impl(std::unique_ptr<TextureUploader> textures, SelectionArt::Logger logger)
        : uploader(std::move(textures)), log(std::move(logger)), worker([this] { Work(); }) {}
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
                    if (haveFactory) pixels = Decode(factory.Get(), job.key, job.paths, job.side, job.entry);
                    else { pixels.outcome = Outcome::Failed; pixels.failure = "image decoder unavailable"; }
                } catch (const std::exception& error) {
                    pixels.bgra.clear(); pixels.outcome = Outcome::Failed;
                    pixels.failure = std::string("decode threw: ") + error.what();
                }
                { std::lock_guard<std::mutex> lock(mutex); completed.push_back(std::move(pixels)); }
            }
        }
        if (SUCCEEDED(initialized)) CoUninitialize();
    }
    // A failed attempt is retried, then reported once. An absent image is
    // final at once and reported only where its policy expects one.
    void Failed(Entry& entry, const std::string& key, Outcome outcome, const std::string& failure) {
        const bool retryable = outcome == Outcome::Failed;
        if (retryable && ++entry.failures < MaximumAttempts) {
            entry.state = State::Waiting;
            entry.retryFrame = frame + RetryFrames * entry.failures;
            return;
        }
        entry.state = State::Missing;
        if (!log) return;
        if (retryable) log("Selection art " + key + " unavailable after " + std::to_string(entry.failures) + " attempts: " + failure);
        else if (entry.policy.reportAbsence) log("Selection art " + key + " unavailable: no game or package file");
    }
};

SelectionArt::SelectionArt(std::unique_ptr<TextureUploader> uploader, std::wstring game, std::wstring assets, Logger log)
    : gameRoot_(std::move(game)), assetRoot_(std::move(assets)), impl_(new Impl(std::move(uploader), std::move(log))) {}
SelectionArt::~SelectionArt() = default;
SelectionImage SelectionArt::Request(const std::string& key, const Files& files, unsigned side, Policy policy, std::string archiveEntry) {
    auto& state = *impl_;
    state.requested = true;
    auto found = state.entries.find(key);
    const bool due = found != state.entries.end() && found->second.state == Impl::State::Waiting && state.frame >= found->second.retryFrame;
    if (found == state.entries.end() || due) {
        std::vector<std::wstring> candidates = files();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.jobs.size() >= 128) return {};
        if (found == state.entries.end()) { found = state.entries.emplace(key, Impl::Entry{}).first; found->second.policy = policy; }
        found->second.state = Impl::State::Queued;
        state.jobs.push_back({key, std::move(candidates), side, std::move(archiveEntry)}); state.wake.notify_one();
    }
    auto& entry = found->second; entry.lastUse = state.frame;
    SelectionImage result;
    result.texture = entry.texture.id;
    result.uvMin = entry.uvMin; result.uvMax = entry.uvMax;
    result.width = static_cast<int>(entry.width * (entry.uvMax.x - entry.uvMin.x));
    result.height = static_cast<int>(entry.height * (entry.uvMax.y - entry.uvMin.y));
    result.missing = entry.state == Impl::State::Missing;
    return result;
}
std::vector<std::wstring> SelectionArt::PackagedFiles(const std::string& key) const {
    const std::wstring relative(key.begin(), key.end());
    return {assetRoot_ + L"/" + relative + L"-cutout.png", assetRoot_ + L"/" + relative + L".png", assetRoot_ + L"/" + relative + L".jpg"};
}
SelectionImage SelectionArt::MenuBackdrop() {
    return Request("ember-menu-background",[&]{ return std::vector<std::wstring>{assetRoot_+L"/../"+ember::Product().branding.menuBackdrop}; },2048);
}
SelectionImage SelectionArt::InputPrompt(const std::string& name) {
    return Request("input-"+name,[&]{ return std::vector<std::wstring>{assetRoot_+L"/../input/"+std::wstring(name.begin(),name.end())+L".png"}; },128);
}
SelectionImage SelectionArt::DeveloperMark() {
    Policy shipped; shipped.reportAbsence = true;
    return Request("developer-mark", [&] {
        return std::vector<std::wstring>{assetRoot_ + L"/../input/mark_developer.png"};
    }, NativeSide, shipped);
}
SelectionImage SelectionArt::Flag(const CountryCode& country) {
    if (!country.Present()) return {};
    Policy atlas; atlas.reportAbsence = true;
    atlas.exactWidth = countries::AtlasWidth; atlas.exactHeight = countries::AtlasHeight;
    auto image = Request("flags/atlas", [&] { return std::vector<std::wstring>{assetRoot_ + L"/../flags/flags.png"}; }, NativeSide, atlas);
    if (!image.texture) return image;
    // The cell, inset half a texel so a flag drawn smaller never samples its
    // neighbour or the transparent padding.
    const float x = float(country.Index() % countries::Columns * countries::SlotWidth + countries::Padding);
    const float y = float(country.Index() / countries::Columns * countries::SlotHeight + countries::Padding);
    image.uvMin = ImVec2((x + .5f) / countries::AtlasWidth, (y + .5f) / countries::AtlasHeight);
    image.uvMax = ImVec2((x + countries::CellWidth - .5f) / countries::AtlasWidth, (y + countries::CellHeight - .5f) / countries::AtlasHeight);
    image.width = countries::CellWidth; image.height = countries::CellHeight;
    return image;
}
void SelectionArt::Pump() {
    auto& state = *impl_;
    if (!state.requested) return;
    state.requested = false; ++state.frame;
    for (int upload = 0; upload < 2; ++upload) {
        Pixels pixels;
        { std::lock_guard<std::mutex> lock(state.mutex);
          if (state.completed.empty()) break;
          pixels = std::move(state.completed.front()); state.completed.pop_front(); }
        state.wake.notify_one();
        auto& entry = state.entries[pixels.key];
        if (pixels.outcome != Outcome::Decoded) { state.Failed(entry, pixels.key, pixels.outcome, pixels.failure); continue; }
        if (entry.policy.exactWidth && (pixels.width != entry.policy.exactWidth || pixels.height != entry.policy.exactHeight)) {
            state.Failed(entry, pixels.key, Outcome::Failed, std::to_string(pixels.width) + "x" + std::to_string(pixels.height) + ", not " +
                std::to_string(entry.policy.exactWidth) + "x" + std::to_string(entry.policy.exactHeight));
            continue;
        }
        std::string failure;
        const ImTextureID texture = state.uploader->Create(pixels.width, pixels.height, pixels.bgra.data(), failure);
        if (!texture) { state.Failed(entry, pixels.key, Outcome::Failed, failure); continue; }
        entry.state = Impl::State::Ready;
        // A later candidate stood in for one that exists but failed, such as
        // the packaged cutout for an unreadable game portrait. Say so once.
        if (!pixels.failure.empty() && state.log) state.log("Selection art " + pixels.key + " uses a fallback: " + pixels.failure);
        entry.texture = Impl::Texture(state.uploader.get(), texture); entry.width = pixels.width; entry.height = pixels.height;
        // Fit the painted portrait, rather than its large transparent canvas.
        // The source pixels stay unchanged; only the UI's texture coordinates differ.
        if (entry.policy.cropToContent) {
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
} }
