#pragma once
// Training, the replay archive and in-game themes of a game that has none
// (its Capabilities::training and replays are off, its
// SettingsFeatures::gameThemes too). The runtime may still call
// these services, and each answers that there is nothing: no Training to
// enter, no archive to list, keep or save. A game without the feature hands
// one of these to its Adapter; the runtime's test game uses NoReplays.

#include "sf4e__GameAdapter.hxx"

namespace ember { namespace game {

class NoTraining final : public Training {
public:
    void Request(const sf4e::netplay::Generation&) override {}
    bool ControlsAvailable() override { return false; }
    bool MatchPracticeActive() override { return false; }
    sf4e::training::Place ReadPlace() override { return {}; }
    void SetCall(const sf4e::training::CallControl&) override {}
    void StopCapture() override {}
};

class NoReplays final : public Replays {
public:
    bool Ready() override { return false; }
    const sf4e::replaystore::Status& Status() override { static const sf4e::replaystore::Status status; return status; }
    void Start(const sf4e::replay::Request&, bool, bool) override {}
    void Tick(bool, bool) override {}
    void TickPlayback(int, int, bool) override {}
    const sf4e::replaytransport::View& Playback() override { static const sf4e::replaytransport::View view; return view; }
    std::shared_ptr<const sf4e::replaylane::Lanes> PlaybackLanes() override { return nullptr; }
    std::filesystem::path ArchivePath() override { return {}; }
    void WantListing() override {}
    std::shared_ptr<const std::vector<sf4e::platform::replays::ArchivedReplay>> LatestListing() override { return nullptr; }
    void WantDetail(const std::string&, std::uint64_t) override {}
    sf4e::replayinputs::DetailCompletion LatestDetail() override { return {}; }
    void StopListing() override {}
    void MarkKept(const std::filesystem::path&, bool) override {}
    void MarkSent(const std::filesystem::path&) override {}
    void MarkSetClosed(const std::filesystem::path&) override {}
    std::filesystem::path SetPackPath(const std::string&) override { return {}; }
    std::size_t ImportReplayFile(const std::filesystem::path&) override { return 0; }
    std::filesystem::path ArchiveWatchPath(const std::string&) override { return {}; }
    void PruneArchiveWatching(const std::filesystem::path&) override {}
    void ClearArchiveWatching() override {}
};

class NoThemes final : public Themes {
public:
    void Start() override {}
    void Tick() override {}
    void TurnOff(std::array<std::string, sf4e::netplay::GameThemePartCount>&) override {}
    const std::vector<sf4e::netplay::GameThemeTurnedOff>& TurnedOff() override { static const std::vector<sf4e::netplay::GameThemeTurnedOff> none; return none; }
    void TurnedOffTold() override {}
    const std::vector<std::string>& Names(sf4e::netplay::GameThemePart) override { static const std::vector<std::string> none; return none; }
    sf4e::netplay::GameThemeNote Note(sf4e::netplay::GameThemePart, const std::string&) override { return sf4e::netplay::GameThemeNote::None; }
    void Use(const std::array<std::string, sf4e::netplay::GameThemePartCount>&) override {}
};

} } // namespace ember::game
