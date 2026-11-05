#pragma once
// Training and the replay archive of a game that has neither (its
// Capabilities::training and replays are off). The runtime may still call
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
    std::size_t ImportSetPack(const std::filesystem::path&) override { return 0; }
};

} } // namespace ember::game
