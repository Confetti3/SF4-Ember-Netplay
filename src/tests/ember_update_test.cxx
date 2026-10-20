#include "../launcher/update/github_release_client_internal.hxx"
#include "../launcher/update/github_release_download.hxx"
#include "../common/ReleaseNotesText.hxx"
#include "../ui/UpdateChannelPick.hxx"
#include "../ui/VersionLine.hxx"
#include "../launcher/update/ZipExtract.hxx"
#include <zlib.h>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdlib>
#include <iostream>

#include "test_support.hxx"
#include "temp_root.hxx"

namespace {
std::string U16(unsigned v) { return std::string{char(v & 255), char(v >> 8)}; }
std::string U32(unsigned long v) { return U16(unsigned(v & 0xffff)) + U16(unsigned(v >> 16)); }
// A minimal zip writer: stored or raw-deflate entries, central directory, no zip64.
struct ZipBuilder {
    std::string bytes, directory; unsigned entries = 0;
    void Add(const std::string& name, const std::string& data, bool deflated) {
        std::string packed = data;
        if (deflated) {
            z_stream z{}; deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            packed.resize(deflateBound(&z, static_cast<uLong>(data.size())));
            z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data())); z.avail_in = static_cast<uInt>(data.size());
            z.next_out = reinterpret_cast<Bytef*>(packed.data()); z.avail_out = static_cast<uInt>(packed.size());
            deflate(&z, Z_FINISH); packed.resize(z.total_out); deflateEnd(&z);
        }
        const auto crc = crc32(0, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size()));
        const std::string common = U16(20) + U16(0) + U16(deflated ? 8 : 0) + U32(0) + U32(crc) + U32(packed.size()) + U32(data.size()) + U16(unsigned(name.size())) + U16(0);
        directory += U32(0x02014b50) + U16(20) + common + U16(0) + U16(0) + U16(0) + U32(0) + U32(bytes.size()) + name;
        bytes += U32(0x04034b50) + common + name + packed;
        ++entries;
    }
    void Write(const std::filesystem::path& path) {
        std::ofstream(path, std::ios::binary) << bytes << directory << U32(0x06054b50) << U16(0) << U16(0) << U16(entries) << U16(entries)
            << U32(directory.size()) << U32(bytes.size()) << U16(0);
    }
};
std::string Slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(in), {});
}
}

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
    // The updater's channel row: Left and Right pick without saving and stop
    // at both ends, so Beta to Stable never passes through Nightly.
    {
        using sf4e::ui::ChannelMove;
        using sf4e::ui::ClassifyChannelMove;
        sf4e::ui::ChannelPick pick;
        CHECK(pick.Shown(UpdateChannel::Beta) == UpdateChannel::Beta && !pick.Pending(UpdateChannel::Beta));
        pick.Step(UpdateChannel::Beta, -1);
        CHECK(pick.Shown(UpdateChannel::Beta) == UpdateChannel::Stable && pick.Pending(UpdateChannel::Beta));
        pick.Step(UpdateChannel::Beta, -1);
        CHECK(pick.Shown(UpdateChannel::Beta) == UpdateChannel::Stable);
        pick.Step(UpdateChannel::Beta, 1);
        CHECK(!pick.picked && !pick.Pending(UpdateChannel::Beta));
        pick.Step(UpdateChannel::Beta, 1); pick.Step(UpdateChannel::Beta, 1);
        CHECK(pick.Shown(UpdateChannel::Beta) == UpdateChannel::Nightly);
        // An unconfirmed pick survives idle frames; a channel saved some other way drops it.
        pick.Settle(UpdateChannel::Beta, false);
        CHECK(pick.Shown(UpdateChannel::Beta) == UpdateChannel::Nightly);
        pick.Settle(UpdateChannel::Stable, false);
        CHECK(!pick.picked && pick.Shown(UpdateChannel::Stable) == UpdateChannel::Stable);
        // A confirmed pick stays on the row while it is saved and checked, then gives way to the saved channel.
        pick.Step(UpdateChannel::Stable, 1); pick.applying = true;
        pick.Settle(UpdateChannel::Stable, true);
        CHECK(pick.Shown(UpdateChannel::Stable) == UpdateChannel::Beta);
        pick.Settle(UpdateChannel::Beta, false);
        CHECK(!pick.picked && !pick.applying && pick.Shown(UpdateChannel::Beta) == UpdateChannel::Beta);
        // A switch that failed (still the old channel, no longer busy) shows the saved channel again.
        pick.Step(UpdateChannel::Beta, 1); pick.applying = true;
        pick.Settle(UpdateChannel::Beta, false);
        CHECK(!pick.picked && !pick.applying);
        CHECK(ClassifyChannelMove(UpdateChannel::Beta, UpdateChannel::Beta) == ChannelMove::None);
        CHECK(ClassifyChannelMove(UpdateChannel::Stable, UpdateChannel::Beta) == ChannelMove::Forward);
        CHECK(ClassifyChannelMove(UpdateChannel::Stable, UpdateChannel::Nightly) == ChannelMove::ToNightly);
        CHECK(ClassifyChannelMove(UpdateChannel::Beta, UpdateChannel::Nightly) == ChannelMove::ToNightly);
        CHECK(ClassifyChannelMove(UpdateChannel::Beta, UpdateChannel::Stable) == ChannelMove::GoesBack);
        CHECK(ClassifyChannelMove(UpdateChannel::Nightly, UpdateChannel::Beta) == ChannelMove::GoesBack);
        CHECK(ClassifyChannelMove(UpdateChannel::Nightly, UpdateChannel::Stable) == ChannelMove::GoesBack);
    }
    // One line names the version and channel on Home, in the launcher window and in Help and about.
    CHECK(sf4e::ui::VersionLine("1.2.0", UpdateChannel::Nightly) == "Ember 1.2.0 \xC2\xB7 Nightly");
    CHECK(sf4e::ui::VersionLine("1.1.2", UpdateChannel::Stable) == "Ember 1.1.2 \xC2\xB7 Stable");
    CHECK(sf4e::ui::VersionLine("1.2.0-rc1", UpdateChannel::Beta) == "Ember 1.2.0-rc1 \xC2\xB7 Beta");
    CHECK(sf4e::ui::VersionLine("", UpdateChannel::Stable).empty());
    // Release notes read as plain text: Markdown marks go, links read as
    // their text, and what is left is only ever text, never markup or a format.
    {
        using sf4e::updates::PlainReleaseNotes;
        const std::string notes =
            "## What's new\r\n\r\n"
            "- **Bold** fix for [rooms](https://x.invalid/y) and `code`\r\n"
            "  * nested _item_ with snake_case_name\r\n\r\n\r\n\r\n"
            "> quoted\r\n---\r\n"
            "<!-- hidden -->Visible<br>line &amp; more\r\n"
            "![shot](https://x.invalid/img.png)\r\n"
            "```\r\nraw *code*\r\n```\r\n"
            "2 * 3 = 6 and 100%s %n {0} \\*kept\\*\r\n"
            "| a | b |\r\n|---|---|\r\n| 1 | 2 |\r\n"
            "Intro\r\n### Fixes ###\r\n+ one\r\n";
        const std::string plain = PlainReleaseNotes(notes);
        const std::string expected =
            "What's new\n\n"
            "- Bold fix for rooms and code\n"
            "  - nested item with snake_case_name\n\n"
            "quoted\n\n"
            "Visible line & more\n"
            "shot\n"
            "raw *code*\n"
            "2 * 3 = 6 and 100%s %n {0} *kept*\n"
            "a, b\n1, 2\n"
            "Intro\n\nFixes\n- one";
        if (plain != expected) std::cerr << "Plain release notes:\n" << plain << '\n';
        CHECK(plain == expected);
        // The client's 2,000-byte cut can split a character; emoji and their
        // joiners are past what the fonts draw; control characters go.
        CHECK(PlainReleaseNotes("caf\xC3\xA9 \xF0\x9F\x8E\x89 done\x07\tnow \xE2\x9C\x94\xEF\xB8\x8F...\xE2\x80") ==
            "caf\xC3\xA9  done now \xE2\x9C\x94...");
        CHECK(PlainReleaseNotes("\xC0\xAF\xED\xA0\x80ok") == "ok");
        CHECK(PlainReleaseNotes("") == "" && PlainReleaseNotes("\r\n\r\n<!-- only a comment -->\r\n") == "");
        CHECK(PlainReleaseNotes("Unclosed [link and **bold and `tick") == "Unclosed [link and **bold and `tick");
        // Comments are taken out only where Markdown has them: a fenced
        // example and an inline code span keep theirs, an unclosed one inside
        // a fence takes nothing after it, and one that never closes, or would
        // close only past a fence, is text.
        CHECK(PlainReleaseNotes("Before\n```html\n<!-- unclosed in code\n```\n- Later entry\n") == "Before\n<!-- unclosed in code\n- Later entry");
        CHECK(PlainReleaseNotes("Use `<!-- note -->` in a template") == "Use <!-- note --> in a template");
        CHECK(PlainReleaseNotes("Keep <!-- this open\n- Next entry") == "Keep <!-- this open\n- Next entry");
        // Comments, code and fences are read in one traversal: an unclosed
        // comment with a '>' after it stays whole, no later pass taking it for
        // a tag; a fence closes only on its own marker, at least as long, so a
        // four-backtick fence keeps a three-backtick example and the comment
        // in it, and a tilde line does not close a backtick fence.
        CHECK(PlainReleaseNotes("Keep <!-- this > open") == "Keep <!-- this > open");
        CHECK(PlainReleaseNotes("Example:\n````md\n```html\n<!-- literal -->\n```\n````\nAfter <!-- gone -->it") ==
            "Example:\n```html\n<!-- literal -->\n```\nAfter it");
        CHECK(PlainReleaseNotes("```\ncode\n~~~\nstill code\n```\nafter") == "code\n~~~\nstill code\nafter");
        CHECK(PlainReleaseNotes("~~~~\na\n~~~\nb\n~~~~\nc") == "a\n~~~\nb\nc");
        // A fence never left open by a comment: one that would close past a fence is text.
        CHECK(PlainReleaseNotes("x <!-- a\n````\n-->\n````\ny") == "x <!-- a\n-->\ny");
        // Code spans close on their own length; a run that closes none is text,
        // and neither is read as markup afterwards.
        CHECK(PlainReleaseNotes("``a ` b`` and `<b>` and ``` lone") == "a ` b and <b> and ``` lone");
        CHECK(PlainReleaseNotes("| `a|b` | c |") == "a|b, c");
        CHECK(PlainReleaseNotes("\\`not code\\` *x*") == "`not code` x");
        CHECK(PlainReleaseNotes("A <!-- opened\n```\ncode -->\n```\nB") == "A <!-- opened\ncode -->\nB");
        CHECK(PlainReleaseNotes("One <!-- across\ntwo --> three\nFour") == "One\nthree\nFour");
        // A comment that never closes is text from its opener to the next
        // fence or the end: no tag, emphasis or list mark in it is read.
        CHECK(PlainReleaseNotes("Keep <!-- <b>literal</b> *x* [y](z)") == "Keep <!-- <b>literal</b> *x* [y](z)");
        CHECK(PlainReleaseNotes("a *em* <!-- <i>one</i>\n- **two**\n# three\n```\ncode\n```\nafter *em*") ==
            "a em <!-- <i>one</i>\n- **two**\n# three\ncode\nafter em");
        // A heading's closing hashes go only apart from its words.
        CHECK(PlainReleaseNotes("## C#") == "C#" && PlainReleaseNotes("## C# ##") == "C#" && PlainReleaseNotes("### Fixes ###") == "Fixes" &&
            PlainReleaseNotes("# F# and C#") == "F# and C#");
        // Formatting controls draw nothing and can turn the text around them:
        // bidirectional embeddings, overrides and isolates, the soft hyphen,
        // the Arabic letter mark and the interlinear marks all go.
        for (const char* control : {"\xE2\x80\xAA", "\xE2\x80\xAB", "\xE2\x80\xAC", "\xE2\x80\xAD", "\xE2\x80\xAE", "\xE2\x81\xA6", "\xE2\x81\xA7",
            "\xE2\x81\xA8", "\xE2\x81\xA9", "\xC2\xAD", "\xD8\x9C", "\xEF\xBF\xB9", "\xE2\x80\x8E", "\xEF\xBB\xBF"})
            CHECK(PlainReleaseNotes(std::string("ab") + control + "cd") == "abcd");
        CHECK(PlainReleaseNotes("caf\xC3\xA9 \xE2\x80\x94 \xE6\x97\xA5\xE6\x9C\xAC") == "caf\xC3\xA9 \xE2\x80\x94 \xE6\x97\xA5\xE6\x9C\xAC");
        CHECK(PlainReleaseNotes("over\xF4\x90\x80\x80long") == "overlong");
    }
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
            // A cancel during the first attempt says so and does not go on to
            // the API fallback.
            const std::atomic<bool> cancelled{true};
            unsigned reports = 0;
            CHECK(!detail::DownloadReleaseZip(offer, (root / L"selected.zip").c_str(), error, cancelled,
                [&](UpdateStage, std::uint64_t, std::uint64_t) { ++reports; }));
            CHECK(reports == 0 && error == sf4e::loc::T("update.cancelled"));
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
    if (argc > 1 && std::string(argv[1]) == "--live") {
        result = CheckForUpdate(UpdateChannel::Stable);
        CHECK(result.ok && !result.installedVersion.empty());
        CHECK(result.latestVersion == "v" + result.installedVersion && !result.updateAvailable);
        CHECK(result.expectedSha256.size() == 64);
        CHECK(result.zipDownloadUrl.find("sf4-ember-netplay-" + result.installedVersion + ".zip") != std::string::npos);
        CHECK(result.zipDownloadUrl.find("upgrade-sf4-ember-netplay-") == std::string::npos);
    }
    {
        const auto work = MakeTempRoot(L"ember-zip-extract-test-");
        std::filesystem::create_directories(work);
        const std::atomic<bool> keep{false}, cancel{true};
        std::string error;
        const auto extract = [&](const char* zip, const char* to, const std::atomic<bool>& stop) {
            return ExtractZip((work / zip).wstring(), (work / to).wstring(), error, stop);
        };
        const std::string big(300000, 'x');
        ZipBuilder zip; zip.Add("pkg/", "", false); zip.Add("pkg/a.txt", "stored", false);
        zip.Add("pkg/sub/b.bin", big + "tail", true); zip.Add("pkg/empty", "", true);
        // One byte past a 64 KiB output buffer: zlib 1.3.1 takes all of this
        // entry's input while that byte is still pending.
        const std::string edge = std::string(65533, 'x') + "tail";
        zip.Add("pkg/edge.bin", edge, true);
        zip.Write(work / "ok.zip");
        CHECK(extract("ok.zip", "out", keep));
        CHECK(Slurp(work / "out/pkg/a.txt") == "stored" && Slurp(work / "out/pkg/sub/b.bin") == big + "tail" && Slurp(work / "out/pkg/empty").empty());
        CHECK(Slurp(work / "out/pkg/edge.bin") == edge);
        CHECK(!extract("ok.zip", "cancel", cancel) && error == "cancelled: pkg/a.txt");
        // Names the installer refuses are refused here too: a way out, and one Windows would alter.
        ZipBuilder evil; evil.Add("../escape.txt", "x", false); evil.Write(work / "evil.zip");
        CHECK(!extract("evil.zip", "out2", keep) && error.find("unsafe entry name") == 0);
        CHECK(!std::filesystem::exists(work / "escape.txt"));
        ZipBuilder dot; dot.Add("pkg/name.", "x", false); dot.Write(work / "dot.zip");
        CHECK(!extract("dot.zip", "out3", keep) && error.find("unsafe entry name") == 0);
        ZipBuilder bad; bad.Add("c.txt", "hello", true); bad.bytes[bad.bytes.size() - 1] ^= 1; bad.Write(work / "bad.zip");
        CHECK(!extract("bad.zip", "out4", keep) && !error.empty());
        RemoveTempRoot(work);
    }
    std::cout << "Ember release identity, asset selection, version comparison, channels and checksum parsing and zip extraction passed\n";
}
