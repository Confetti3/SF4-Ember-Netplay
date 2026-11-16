#pragma once
#include <array>
#include <string_view>

namespace sf4e { namespace platform {
// Community Discord server, shown in Help & About and opened as https://<invite>.
constexpr const char* CommunityInvite = "discord.gg/uPNqF5A5uq";
// The mark a link wears on Home's corner. The interface maps each to what draws it.
enum class LinkMark { Discord, Coffee, Ember, Twitter };
// One of Ember's outbound links. `address` is what the browser opens as https://<address>
// and what the rows name; `label` and `detail` are catalog ids, `detail` taking the address as {0}.
struct Link {
    const char* id;
    const char* address;
    LinkMark mark;
    const char* label;
    const char* detail;
};
// In the order Help & About lists them and Home's corner draws them. Ko-fi has no logo in the
// icon set, so its mark is the cup (LinkMark::Coffee).
inline constexpr std::array<Link, 4> Links = {{
    {"discord", CommunityInvite, LinkMark::Discord, "about.discord", "about.discord_detail"},
    {"kofi", "ko-fi.com/confetti3", LinkMark::Coffee, "about.kofi", "about.kofi_detail"},
    {"website", "embernetplay.link", LinkMark::Ember, "about.website", "about.website_detail"},
    {"twitter", "x.com/kabiefox", LinkMark::Twitter, "about.twitter", "about.twitter_detail"},
}};
// The link named `id`, or null.
inline const Link* FindLink(std::string_view id) {
    for (const Link& link : Links) if (id == link.id) return &link;
    return nullptr;
}
} }
