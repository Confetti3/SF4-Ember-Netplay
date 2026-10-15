#pragma once

#include "github_release_client.hxx"

namespace sf4e { namespace launcher { namespace detail {

// The selected assets and their release page stay together through download.
// On failure, outError includes the offer's page for a manual download. A
// cancel stops the transfer and skips any further attempt.
bool DownloadReleaseZip(
    const UpdateCheckResult& offer, const wchar_t* zipPath, std::string& outError,
    const std::atomic<bool>& cancel = NeverCancelled, const Progress& progress = {}
);

} } } // namespace sf4e::launcher::detail
