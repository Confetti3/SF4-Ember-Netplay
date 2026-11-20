// The menu backdrop's movie frames between the game and the launcher
// (MovieFrameMailbox.hxx, MovieFrameService.hxx): a request goes from the
// game's end to the launcher's and its answer back, a stale answer never
// reaches a newer request, the service refuses a frame that is not on the
// list (an ending among them), answers a frame already in the cache at once
// and one whose movie is missing as failed. With the game's folder in
// SF4E_GAME_DIR it also decodes a listed frame into a JPEG; without it, or
// without Media Foundation, that part is skipped.
#include "../common/MovieFrames.hxx"
#include "../platform/MovieFrameMailbox.hxx"
#include "../platform/MovieFrameService.hxx"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "test_support.hxx"

namespace mailbox = sf4e::platform::movie_frames;
namespace frames = sf4e::movie_frames;

namespace {

// A name of this test's own, so it never meets a running game.
std::wstring TestName(const wchar_t* part) {
    return L"Local\\SF4EmberMovieFrameTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + part;
}

// Waits up to `seconds` for an answer to `request`.
mailbox::Answer AnswerOf(mailbox::MovieFrameClient& client, std::uint32_t request, double seconds = 10) {
    const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000);
    for (;;) {
        const mailbox::Answer answer = client.Read(request);
        if (answer != mailbox::Answer::None || GetTickCount64() > until) return answer;
        Sleep(10);
    }
}

void Mailbox() {
    const std::wstring name = TestName(L"mailbox");
    mailbox::MovieFrameClient absent;
    EXPECT(!absent.Open(name) && !absent.Opened() && absent.Ask("movie/prologue/RYU.wmv", 18000) == 0);
    mailbox::MovieFrameHost host;
    CHECK(host.Create(name));
    // A second launcher for the same game is refused rather than sharing the slot.
    mailbox::MovieFrameHost twin;
    EXPECT(!twin.Create(name));
    mailbox::MovieFrameClient client;
    CHECK(client.Open(name));
    EXPECT(client.Read(0) == mailbox::Answer::None);
    const std::uint32_t first = client.Ask("movie/prologue/RYU.wmv", 18000);
    EXPECT(first != 0);
    EXPECT(WaitForSingleObject(host.ReadyEvent(), 1000) == WAIT_OBJECT_0);
    std::uint32_t request = 0, ms = 0;
    std::string movie;
    EXPECT(host.Take(request, movie, ms) && request == first && movie == "movie/prologue/RYU.wmv" && ms == 18000);
    EXPECT(client.Read(first) == mailbox::Answer::None);
    host.Reply(first, mailbox::Answer::Ready);
    EXPECT(client.Read(first) == mailbox::Answer::Ready);
    // Answered, the slot holds no request to take again.
    EXPECT(!host.Take(request, movie, ms));
    // A newer request never gets the answer meant for the one before.
    const std::uint32_t second = client.Ask("movie/opening/opening_r.wmv", 90000);
    EXPECT(second != 0 && second != first);
    host.Reply(first, mailbox::Answer::Failed);
    EXPECT(client.Read(second) == mailbox::Answer::None);
    host.Reply(second, mailbox::Answer::Unavailable);
    EXPECT(client.Read(second) == mailbox::Answer::Unavailable && client.Read(first) == mailbox::Answer::None);
    // A path longer than the slot is not sent.
    EXPECT(client.Ask(std::string(200, 'a').c_str(), 1) == 0);
}

// The service with a game folder that has no movies.
void Service(const std::filesystem::path& root) {
    const std::filesystem::path game = root / L"game", cache = root / L"cache";
    std::filesystem::create_directories(game);
    std::filesystem::create_directories(cache);
    const std::wstring name = TestName(L"service");
    std::vector<std::string> lines;
    mailbox::MovieFrameService service;
    CHECK(service.Start(GetCurrentProcessId(), game.wstring(), [&](const std::string& line) { lines.push_back(line); }, name, cache.wstring()));
    {
        mailbox::MovieFrameClient client;
        CHECK(client.Open(name));
        // Not on the list: an ending, or a listed movie at another time.
        EXPECT(AnswerOf(client, client.Ask("movie/ending/RYU.wmv", 27000)) == mailbox::Answer::Failed);
        EXPECT(AnswerOf(client, client.Ask(frames::Curated()[0].movie, frames::Curated()[0].ms + 1)) == mailbox::Answer::Failed);
        // Already in the cache: Ready, nothing decoded.
        const auto& cached = frames::Curated()[1];
        { std::ofstream(cache / frames::FileName(cached), std::ios::binary) << "jpeg"; }
        EXPECT(AnswerOf(client, client.Ask(cached.movie, cached.ms)) == mailbox::Answer::Ready);
        // Its movie missing: this frame failed (or no frame decodes at all here).
        const auto& missing = frames::Curated()[2];
        const mailbox::Answer answer = AnswerOf(client, client.Ask(missing.movie, missing.ms));
        EXPECT(answer == mailbox::Answer::Failed || answer == mailbox::Answer::Unavailable);
        EXPECT(!std::filesystem::exists(cache / frames::FileName(missing)));
    }
    service.Stop();
    // Stopped (and the game's end closed), the mailbox is gone with it.
    mailbox::MovieFrameClient late;
    EXPECT(!late.Open(name));
}

// A real decode, with the game's folder known.
void Decode(const std::filesystem::path& root) {
    const char* folder = std::getenv("SF4E_GAME_DIR");
    if (!folder || !*folder) { std::cout << "SF4E_GAME_DIR is not set: no movie decoded\n"; return; }
    const std::filesystem::path cache = root / L"decoded";
    const std::wstring name = TestName(L"decode");
    mailbox::MovieFrameService service;
    CHECK(service.Start(GetCurrentProcessId(), std::filesystem::path(folder).wstring(), [](const std::string& line) { std::cout << line << "\n"; },
        name, cache.wstring()));
    mailbox::MovieFrameClient client;
    CHECK(client.Open(name));
    const auto& frame = frames::Curated()[0];
    const mailbox::Answer answer = AnswerOf(client, client.Ask(frame.movie, frame.ms), 60);
    if (answer == mailbox::Answer::Unavailable) { std::cout << "Media Foundation cannot decode the movies here: decode skipped\n"; return; }
    EXPECT(answer == mailbox::Answer::Ready);
    std::ifstream file(cache / frames::FileName(frame), std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // A JPEG of a whole frame, not a black one: a 1280x720 frame of the
    // anime is tens of kilobytes at quality 90.
    EXPECT(bytes.size() > 20000 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xD8);
    // No temporary file is left beside it.
    for (const auto& entry : std::filesystem::directory_iterator(cache)) EXPECT(entry.path().extension() == L".jpg");
}

} // namespace

int main() {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path root = std::filesystem::path(temp) / (L"sf4e-movie-frames-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    Mailbox();
    Service(root);
    Decode(root);
    std::filesystem::remove_all(root, error);
    return TEST_EXIT_CODE();
}
