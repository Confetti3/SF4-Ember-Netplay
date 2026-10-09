#include "../launcher/update/github_release_client_internal.hxx"
#include "../launcher/update/github_release_download.hxx"
#include <filesystem>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>

#include "test_support.hxx"
#include "temp_root.hxx"

int main(int argc, char** argv) {
    using namespace sf4e::launcher;
    CHECK(std::string(kDefaultGithubRepo) == "Confetti3/SF4-Ember-Netplay");
    const std::string digest(64, 'a');
    nlohmann::json release = {
        {"tag_name", "v0.8.3"}, {"body", "Ember 0.8.3"},
        {"html_url", "https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v0.8.3"},
        {"assets", nlohmann::json::array({
            {{"name", "sf4-netplay-launcher-0.6.5.zip"}, {"browser_download_url", "legacy"}},
            {{"name", "sf4-ember-netplay-0.8.3.zip.sha256"}, {"browser_download_url", "checksum"}},
            {{"name", "upgrade-sf4-ember-netplay-0.8.2-to-0.8.3.zip"}, {"browser_download_url", "incremental"},
             {"url", "upgrade-api"}, {"digest", "sha256:" + digest}},
            {{"name", "sf4-ember-netplay-0.8.3.zip"}, {"browser_download_url", "branded"},
             {"url", "api-asset"}, {"digest", "sha256:" + digest}}
        })}
    };
    const auto one = [&](const char* installed, UpdateChannel channel = UpdateChannel::Stable) {
        return ParseGithubReleases(nlohmann::json::array({release}).dump(), installed, channel);
    };
    auto result = one("0.8.2");
    CHECK(result.ok && result.updateAvailable && result.latestVersion == "v0.8.3");
    CHECK(result.zipDownloadUrl == "branded" && result.zipApiUrl == "api-asset");
    CHECK(result.expectedSha256 == digest);
    CHECK(!one("0.8.3").updateAvailable);
    CHECK(!one("0.9.0").updateAvailable);
    CHECK(one("dev").updateAvailable); // A build without a version label is offered the best release.
    release["assets"][3].erase("digest");
    CHECK(one("0.8.2").expectedSha256.empty());
    release["assets"].erase(3);
    CHECK(!one("0.8.2").ok);
    CHECK(!ParseGithubReleases("invalid json", "0.8.3", UpdateChannel::Stable).ok);
    CHECK(!ParseGithubReleases("{}", "0.8.3", UpdateChannel::Stable).ok);
    CHECK(!ParseGithubReleases("[]", "0.8.3", UpdateChannel::Stable).ok);
    CHECK(!ParseGithubReleases("{\"message\":\"rate limited\"}", "0.8.3", UpdateChannel::Stable).ok);

    // Versions: a pre-release sorts before its finished release, then by word,
    // number and the rest; anything of another shape is not a version.
    const auto before = [](const char* a, const char* b) {
        const auto x = ParseVersion(a), y = ParseVersion(b);
        return x && y && CompareVersions(*x, *y) < 0 && CompareVersions(*y, *x) > 0;
    };
    CHECK(before("1.1.0-rc1", "v1.1.0") && before("1.0.2", "1.1.0-rc1") && before("v1.1.0", "1.1.1"));
    CHECK(before("1.1.0-rc1", "1.1.0-rc2") && before("1.1.0-rc9", "1.1.0-rc10") && before("1.1.0-beta3", "1.1.0-rc1"));
    CHECK(before("1.1.0-rc1", "1.1.0-rc1junk") && CompareVersions(*ParseVersion("v1.1.0-RC1"), *ParseVersion("1.1.0-rc1")) == 0);
    CHECK(ParseVersion("1.1.0-links-sets-test1") && ParseVersion("1.1.0-links-sets-test1")->prerelease);
    for (const char* bad : {"latest", "v1.1.0-", "1.1", "1.1.0.0", "1.1.0 ", "v1.1.0-rc 1", "1234567890.0.0", "", "rc1"}) CHECK(!ParseVersion(bad));
    CHECK(!ParseVersion(nullptr));
    CHECK(before("1.2.0-nightly20261008.2", "1.2.0-nightly20261008.10"));
    CHECK(before("1.2.0-NIGHTLY20261008.2", "1.2.0-nightly20261008.10"));
    CHECK(before("1.2.0-nightly20261008.2.9", "1.2.0-nightly20261008.2.10"));
    CHECK(before("1.2.0-nightly20261008.99999999999999999999", "1.2.0-nightly20261008.100000000000000000000"));
    CHECK(CompareVersions(*ParseVersion("1.2.0-nightly20261008.02"), *ParseVersion("1.2.0-nightly20261008.2")) == 0);
    CHECK(before("1.2.0-nightly20261008.02", "1.2.0-nightly20261008.1a"));
    // Other pre-release words retain their existing suffix ordering.
    CHECK(before("1.2.0-beta1.10", "1.2.0-beta1.2"));

    for (const auto& info : kUpdateChannels) {
        CHECK(ParseSavedUpdateChannel(info.stored) == info.channel);
        CHECK(ResolveUpdateChannel(ParseSavedUpdateChannel(info.stored), "dev") == info.channel);
    }
    for (const char* saved : {"", "bogus", "beta", "Nightly", " nightly", "stable "})
        CHECK(!ParseSavedUpdateChannel(saved));

    // The chosen channel, or the installed version's kind while none is chosen.
    CHECK(ResolveUpdateChannel(std::nullopt, "1.0.2") == UpdateChannel::Stable && ResolveUpdateChannel(std::nullopt, "1.1.0-rc1") == UpdateChannel::Beta);
    CHECK(ResolveUpdateChannel(std::nullopt, "1.1.0-links-sets-test1") == UpdateChannel::Beta && ResolveUpdateChannel(std::nullopt, "dev") == UpdateChannel::Stable);
    CHECK(ResolveUpdateChannel(ParseSavedUpdateChannel("stable"), "1.1.0-rc1") == UpdateChannel::Stable && ResolveUpdateChannel(ParseSavedUpdateChannel("prerelease"), "1.0.2") == UpdateChannel::Beta);
    CHECK(std::string(UpdateChannelName(UpdateChannel::Stable)) == "stable" && std::string(UpdateChannelName(UpdateChannel::Beta)) == "prerelease");
    CHECK(ResolveUpdateChannel(std::nullopt, "v1.2.0-NIGHTLY20261008.2") == UpdateChannel::Nightly);
    CHECK(ResolveUpdateChannel(ParseSavedUpdateChannel("nightly"), "1.0.2") == UpdateChannel::Nightly);
    CHECK(ResolveUpdateChannel(ParseSavedUpdateChannel("stable"), "1.2.0-nightly20261008") == UpdateChannel::Stable);
    CHECK(ResolveUpdateChannel(ParseSavedUpdateChannel("prerelease"), "1.2.0-nightly20261008") == UpdateChannel::Beta);
    CHECK(ResolveUpdateChannel(ParseSavedUpdateChannel("bogus"), "1.2.0-nightly20261008") == UpdateChannel::Nightly);
    CHECK(ResolveUpdateChannel(std::nullopt, nullptr) == UpdateChannel::Stable);
    CHECK(std::string(UpdateChannelName(UpdateChannel::Nightly)) == "nightly");
    CHECK(std::string(UpdateChannelRepo(UpdateChannel::Stable)) == kDefaultGithubRepo);
    CHECK(std::string(UpdateChannelRepo(UpdateChannel::Beta)) == kDefaultGithubRepo);
    CHECK(std::string(UpdateChannelRepo(UpdateChannel::Nightly)) == "Confetti3/SF4-Ember-Netplay-Nightly");
    CHECK(GetUpdateChannelInfo(UpdateChannel::Stable).next == UpdateChannel::Beta);
    CHECK(GetUpdateChannelInfo(UpdateChannel::Beta).next == UpdateChannel::Nightly);
    CHECK(GetUpdateChannelInfo(UpdateChannel::Nightly).next == UpdateChannel::Stable);
    CHECK(ClassifyReleaseKind(*ParseVersion("v1.2.0")) == ReleaseKind::Stable);
    for (const char* tag : {"v1.2.0-rc1", "v1.2.0-beta2", "v1.2.0-links-sets-test1"})
        CHECK(ClassifyReleaseKind(*ParseVersion(tag)) == ReleaseKind::Beta);
    for (const char* tag : {"v1.2.0-nightly20261008", "v1.2.0-NIGHTLY20261008.2"})
        CHECK(ClassifyReleaseKind(*ParseVersion(tag)) == ReleaseKind::Nightly);
    CHECK(ParseVersion("v1.2.0-nightly20261008.2")->number == 20261008);
    CHECK(ParseVersion("v1.2.0-nightly20261008.2")->rest == ".2");

    // Each channel takes its highest listed release with a package: Stable
    // finished ones only, Beta finished releases and betas, Nightly nightlies.
    // Drafts, packageless releases and tags that are not versions are skipped.
    release["assets"].push_back({{"name", "sf4-ember-netplay-x.zip"}, {"browser_download_url", "branded"}, {"digest", "sha256:" + digest}});
    const auto listed = [&](std::initializer_list<const char*> tags) {
        auto list = nlohmann::json::array();
        for (const char* tag : tags) { release["tag_name"] = tag; list.push_back(release); }
        return list;
    };
    auto list = listed({"v1.1.0-rc1", "v1.0.2", "latest", "v1.1.0-rc2", "v1.0.1"});
    result = ParseGithubReleases(list.dump(), "1.0.2", UpdateChannel::Beta);
    CHECK(result.ok && result.updateAvailable && result.latestVersion == "v1.1.0-rc2" && result.expectedSha256 == digest);
    CHECK(!ParseGithubReleases(list.dump(), "1.1.0-rc2", UpdateChannel::Beta).updateAvailable);
    result = ParseGithubReleases(list.dump(), "1.0.1", UpdateChannel::Stable);
    CHECK(result.ok && result.updateAvailable && result.latestVersion == "v1.0.2");
    // An installed kind the channel does not accept can go back to its best release.
    result = ParseGithubReleases(list.dump(), "1.1.0-rc1", UpdateChannel::Stable);
    CHECK(result.ok && result.updateAvailable && result.goesBack && result.latestVersion == "v1.0.2");
    CHECK(!ParseGithubReleases(list.dump(), "1.0.2", UpdateChannel::Stable).goesBack);
    result = ParseGithubReleases(listed({"v1.0.2"}).dump(), "1.1.0", UpdateChannel::Stable);
    CHECK(result.ok && !result.updateAvailable && !result.goesBack);
    CHECK(!ParseGithubReleases(list.dump(), "1.1.0-rc1", UpdateChannel::Beta).goesBack);
    result = ParseGithubReleases(list.dump(), "1.2.0-nightly20261008", UpdateChannel::Stable);
    CHECK(result.ok && result.updateAvailable && result.goesBack && result.latestVersion == "v1.0.2");
    result = ParseGithubReleases(list.dump(), "1.2.0-nightly20261008", UpdateChannel::Beta);
    CHECK(result.ok && result.updateAvailable && result.goesBack && result.latestVersion == "v1.1.0-rc2");
    result = ParseGithubReleases(list.dump(), "1.2.0", UpdateChannel::Beta);
    CHECK(result.ok && !result.updateAvailable && !result.goesBack);

    // Release list order does not change the best Nightly or its update offer.
    for (const auto tags : {
        std::initializer_list<const char*>{"v1.3.0-nightly20261008.2", "v1.3.0-nightly20261008.10", "v1.3.0-nightly20261008.1"},
        std::initializer_list<const char*>{"v1.3.0-nightly20261008.10", "v1.3.0-nightly20261008.1", "v1.3.0-nightly20261008.2"}}) {
        result = ParseGithubReleases(listed(tags).dump(), "1.3.0-nightly20261008.2", UpdateChannel::Nightly);
        CHECK(result.ok && result.latestVersion == "v1.3.0-nightly20261008.10" && result.updateAvailable && !result.goesBack);
        CHECK(TransitionOffered(result.latestVersion.c_str(), result.installedVersion.c_str(), result.goesBack));
        CHECK(!ParseGithubReleases(listed(tags).dump(), "1.3.0-nightly20261008.10", UpdateChannel::Nightly).updateAvailable);
    }

    // Failed asset downloads retain the selected page, even when its repository
    // cannot be inferred from the tag. Disallowed hosts fail without HTTP.
    {
        const auto root = MakeTempRoot(L"ember-update-download-test-");
        CHECK(std::filesystem::create_directory(root));
        for (const char* page : {
            "https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/selected-beta",
            "https://github.com/Confetti3/SF4-Ember-Netplay-Nightly/releases/tag/selected-nightly",
            "https://github.com/another-owner/selected-repository/releases/tag/selected-release"}) {
            release["html_url"] = page;
            release["tag_name"] = "v1.1.0";
            auto offer = one("1.0.2");
            CHECK(offer.ok && offer.releaseUrl == page);
            offer.zipDownloadUrl = "https://blocked.invalid/selected.zip";
            offer.zipApiUrl = "https://blocked.invalid/assets/selected";
            std::string error;
            CHECK(!detail::DownloadReleaseZip(offer, (root / L"selected.zip").c_str(), error));
            CHECK(error.find(offer.releaseUrl) != std::string::npos);
            CHECK(error.find("host is not allowlisted") != std::string::npos);
            CHECK(!std::filesystem::exists(root / L"selected.zip"));
            // Cancellation between download attempts still names this offer's
            // page, and does not proceed to the API fallback.
            unsigned reports = 0;
            CHECK(!detail::DownloadReleaseZip(offer, (root / L"selected.zip").c_str(), error,
                [&](std::uint64_t done, std::uint64_t total) {
                    ++reports;
                    CHECK(done == 0 && total == 0);
                    return false;
                }));
            CHECK(reports == 1 && error.find("Cancelled") != std::string::npos);
            CHECK(error.find(offer.releaseUrl) != std::string::npos);
            CHECK(error.find("api:") == std::string::npos);
            CHECK(!std::filesystem::exists(root / L"selected.zip"));
        }
        RemoveTempRoot(root);
    }

    auto mixed = listed({"v1.1.0", "v1.2.0-rc1", "v1.3.0-nightly20261008", "v1.3.0-nightly20261008.2", "v2.0.0-nightly20261009"});
    mixed[2]["prerelease"] = true; mixed[3]["prerelease"] = true;
    mixed[4]["assets"] = nlohmann::json::array();
    result = ParseGithubReleases(mixed.dump(), "1.0.2", UpdateChannel::Beta);
    CHECK(result.ok && result.updateAvailable && result.latestVersion == "v1.2.0-rc1");
    mixed[1]["assets"] = nlohmann::json::array();
    CHECK(ParseGithubReleases(mixed.dump(), "1.0.2", UpdateChannel::Beta).latestVersion == "v1.1.0");
    result = ParseGithubReleases(mixed.dump(), "1.3.0-nightly20261008", UpdateChannel::Nightly);
    CHECK(result.ok && result.updateAvailable && !result.goesBack && result.latestVersion == "v1.3.0-nightly20261008.2");
    mixed[3]["draft"] = true;
    CHECK(ParseGithubReleases(mixed.dump(), "1.0.2", UpdateChannel::Nightly).latestVersion == "v1.3.0-nightly20261008");
    mixed[2]["prerelease"] = false;
    CHECK(ParseGithubReleases(mixed.dump(), "1.0.2", UpdateChannel::Nightly).latestVersion == "v1.3.0-nightly20261008");
    mixed[2]["assets"] = nlohmann::json::array();
    CHECK(!ParseGithubReleases(mixed.dump(), "1.0.2", UpdateChannel::Nightly).ok);
    CHECK(!ParseGithubReleases(listed({"v1.2.0-nightly20261008"}).dump(), "1.0.2", UpdateChannel::Beta).ok);
    for (const auto channel : {UpdateChannel::Stable, UpdateChannel::Beta, UpdateChannel::Nightly}) {
        result = ParseGithubReleases(listed({"v1.0.2", "v1.2.0-rc1", "v1.3.0-nightly20261008"}).dump(), "dev", channel);
        CHECK(result.ok && result.updateAvailable && !result.goesBack);
    }
    // An install only takes the kind of change it was offered.
    CHECK(TransitionOffered("v1.1.0", "1.0.2", false) && !TransitionOffered("v1.1.0", "1.0.2", true));
    CHECK(TransitionOffered("v1.0.2", "1.1.0-rc1", true) && !TransitionOffered("v1.0.2", "1.1.0-rc1", false));
    CHECK(TransitionOffered("v1.1.0", "dev", false) && !TransitionOffered("v1.1.0", "dev", true) && !TransitionOffered("latest", "1.1.0-rc1", true));
    list = listed({"v1.1.0", "v1.1.0-rc2"});
    CHECK(ParseGithubReleases(list.dump(), "1.1.0-rc2", UpdateChannel::Beta).latestVersion == "v1.1.0");
    CHECK(ParseGithubReleases(list.dump(), "1.1.0-rc2", UpdateChannel::Stable).updateAvailable);
    list = listed({"v1.2.0-rc1", "v1.1.0", "v1.3.0-rc1"});
    list[0]["draft"] = true; list[2]["assets"] = nlohmann::json::array();
    CHECK(ParseGithubReleases(list.dump(), "1.0.2", UpdateChannel::Beta).latestVersion == "v1.1.0");
    CHECK(!ParseGithubReleases(listed({"latest", "nightly"}).dump(), "1.0.2", UpdateChannel::Beta).ok);
    // GitHub sends null for empty fields: one such release, before or after
    // the newest, spoils nothing (a null digest only leaves it unverifiable).
    list = listed({"v1.0.1", "v1.1.0", "v1.0.0"});
    list[0]["body"] = nullptr; list[2]["body"] = nullptr; list[2]["html_url"] = nullptr;
    for (auto& asset : list[0]["assets"]) asset["digest"] = nullptr;
    result = ParseGithubReleases(list.dump(), "1.0.0", UpdateChannel::Stable);
    CHECK(result.ok && result.latestVersion == "v1.1.0" && result.expectedSha256 == digest);
    list[1]["tag_name"] = nullptr;
    result = ParseGithubReleases(list.dump(), "1.0.0", UpdateChannel::Stable);
    CHECK(result.ok && result.latestVersion == "v1.0.1" && result.expectedSha256.empty());
    // Stable also skips a release GitHub marks as a pre-release, whatever its tag.
    list = listed({"v1.2.0", "v1.1.0"});
    list[0]["prerelease"] = true;
    CHECK(ParseGithubReleases(list.dump(), "1.0.2", UpdateChannel::Stable).latestVersion == "v1.1.0");
    CHECK(ParseGithubReleases(list.dump(), "1.0.2", UpdateChannel::Beta).latestVersion == "v1.2.0");
    // The extraction child still stops when asked: the wait asks each slice with no
    // numbers (the stage has no total), and a false answer ends the child promptly.
    {
        wchar_t system[MAX_PATH] = {};
        CHECK(GetSystemDirectoryW(system, MAX_PATH) > 0);
        const std::wstring cmd = std::wstring(system) + L"\\cmd.exe";
        DWORD exitCode = 0;
        unsigned asked = 0;
        CHECK(detail::RunProcessAndWaitHidden(cmd.c_str(), (L"\"" + cmd + L"\" /c exit 0").c_str(), &exitCode,
            [&](std::uint64_t done, std::uint64_t total) { ++asked; CHECK(done == 0 && total == 0); return true; }));
        CHECK(exitCode == 0);
        asked = 0;
        const auto started = std::chrono::steady_clock::now();
        CHECK(detail::RunProcessAndWaitHidden(cmd.c_str(), (L"\"" + cmd + L"\" /c ping -n 60 127.0.0.1 >nul").c_str(), &exitCode,
            [&](std::uint64_t done, std::uint64_t total) { ++asked; CHECK(done == 0 && total == 0); return asked < 2; }));
        CHECK(exitCode != 0 && asked == 2);
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(20));
    }
    if (argc > 1 && std::string(argv[1]) == "--live") {
        result = CheckForUpdate(UpdateChannel::Stable);
        CHECK(result.ok && !result.installedVersion.empty());
        CHECK(result.latestVersion == "v" + result.installedVersion && !result.updateAvailable);
        CHECK(result.expectedSha256.size() == 64);
        CHECK(result.zipDownloadUrl.find("sf4-ember-netplay-" + result.installedVersion + ".zip") != std::string::npos);
        CHECK(result.zipDownloadUrl.find("upgrade-sf4-ember-netplay-") == std::string::npos);
    }
    std::cout << "Ember release identity, asset selection, version comparison, channels and checksum parsing passed\n";
}
