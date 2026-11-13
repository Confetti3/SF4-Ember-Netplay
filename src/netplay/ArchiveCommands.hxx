#pragma once
// What the interface asks of the public replay archive's browser: one type per
// request, each holding only what it carries and stating its own bounds, taken
// by the runtime like every other command (RuntimeCommands.hxx). The answers
// are read from ShellView::archiveBrowse, or from ShellView::profileSets when
// `profile` is set, the second slot a profile's sets are asked in so they never
// replace the Archive tab's listing.
//
//   ArchiveList   What the tab wants listed: `filter` on the service `bridge`,
//                 the next page too once the player is at the end of the list
//                 (`nearEnd`), and `retry` for a page that failed or a refresh.
//                 Sent when the want or the listing changes; the runtime
//                 decides which page, if any, that asks for (Requests::Ask).
//   ArchiveWatch  The game at `url` (its digest `digest`) is downloaded, and
//                 then watched as `watch` says (no path: the download is it).
#include "Archive.hxx"
#include "../common/ReplayRequest.hxx"

#include <cstddef>
#include <string>

namespace sf4e { namespace commands {

struct ArchiveList {
	std::string bridge;
	netplay::archive::Filter filter;
	bool nearEnd = false, retry = false, profile = false;
	bool operator==(const ArchiveList& other) const {
		return bridge == other.bridge && filter == other.filter && nearEnd == other.nearEnd && retry == other.retry && profile == other.profile;
	}
	bool Valid() const { return bridge.size() <= netplay::archive::MaxBridge && filter.Valid(); }
	std::size_t Bytes() const { return bridge.size() + filter.Bytes(); }
};
struct ArchiveWatch {
	std::string bridge, url, digest;
	replay::Request watch;
	bool profile = false;
	bool Valid() const {
		return bridge.size() <= netplay::archive::MaxBridge && url.size() <= netplay::archive::MaxUrl && digest.size() <= 64 && watch.path.empty() &&
			watch.caption.name[0].size() <= netplay::archive::MaxText && watch.caption.name[1].size() <= netplay::archive::MaxText &&
			watch.caption.text.size() <= netplay::archive::MaxText;
	}
	std::size_t Bytes() const { return bridge.size() + url.size() + digest.size() + watch.caption.name[0].size() + watch.caption.name[1].size() + watch.caption.text.size(); }
};

} }
