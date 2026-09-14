#include "../launcher/update/github_release_client.hxx"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <iostream>

#define CHECK(c) do { if (!(c)) { std::cerr << "Failure at " << __LINE__ << '\n'; std::exit(1); } } while (false)

int main(int argc, char** argv) {
    using namespace sf4e::launcher;
    CHECK(std::string(kDefaultGithubRepo) == "Confetti3/SF4-Ember-Netplay");
    const std::string digest(64, 'a');
    nlohmann::json release = {
        {"tag_name", "v0.8.1"}, {"body", "Ember 0.8.1"},
        {"html_url", "https://github.com/Confetti3/SF4-Ember-Netplay/releases/tag/v0.8.1"},
        {"assets", nlohmann::json::array({
            {{"name", "sf4-netplay-launcher-0.6.5.zip"}, {"browser_download_url", "legacy"}},
            {{"name", "sf4-ember-netplay-0.8.1.zip.sha256"}, {"browser_download_url", "checksum"}},
            {{"name", "sf4-ember-netplay-0.8.1.zip"}, {"browser_download_url", "branded"},
             {"url", "api-asset"}, {"digest", "sha256:" + digest}}
        })}
    };
    auto result = ParseGithubReleaseResponse(release.dump(), "0.8.0");
    CHECK(result.ok && result.updateAvailable && result.latestVersion == "v0.8.1");
    CHECK(result.zipDownloadUrl == "branded" && result.zipApiUrl == "api-asset");
    CHECK(result.expectedSha256 == digest);
    CHECK(!ParseGithubReleaseResponse(release.dump(), "0.8.1").updateAvailable);
    CHECK(!ParseGithubReleaseResponse(release.dump(), "0.9.0").updateAvailable);
    release["assets"][2].erase("digest");
    CHECK(ParseGithubReleaseResponse(release.dump(), "0.8.0").expectedSha256.empty());
    release["assets"].erase(2);
    CHECK(!ParseGithubReleaseResponse(release.dump(), "0.8.0").ok);
    CHECK(!ParseGithubReleaseResponse("invalid json", "0.8.1").ok);
    CHECK(!ParseGithubReleaseResponse("{}", "0.8.1").ok);
    if (argc > 1 && std::string(argv[1]) == "--live") {
        result = CheckForUpdate();
        CHECK(result.ok && result.latestVersion == "v0.8.1" && !result.updateAvailable);
        CHECK(result.expectedSha256.size() == 64);
        CHECK(result.zipDownloadUrl.find("sf4-ember-netplay-0.8.1.zip") != std::string::npos);
    }
    std::cout << "Ember release identity, asset selection, version comparison and checksum parsing passed\n";
}
