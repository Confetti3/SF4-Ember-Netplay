#pragma once

#include "github_release_client.hxx"

namespace sf4e { namespace launcher { namespace detail {

// The selected assets and their release page stay together through download.
// On failure, outError includes the offer's page for a manual download.
bool DownloadReleaseZip(
    const UpdateCheckResult& offer, const wchar_t* zipPath, std::string& outError,
    const std::function<bool(std::uint64_t, std::uint64_t)>& progress = {}
);

} } } // namespace sf4e::launcher::detail
