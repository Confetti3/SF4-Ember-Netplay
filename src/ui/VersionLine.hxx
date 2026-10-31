#pragma once
#include "../common/Localization.hxx"
#include "../common/ReleaseVersion.hxx"
#include <string>

namespace sf4e { namespace ui {
// "Ember 1.2.0 · Stable": the version on this PC and the kind of build it is,
// read as the update check reads a release's tag (updates::ClassifyReleaseKind).
// A Nightly build says Nightly whatever channel its updates follow; the
// channel row says that. Home's corner, the launcher's window and Help and
// about all show this one line. Empty when there is no version to show.
inline std::string VersionLine(const std::string& version) {
    if(version.empty())return {};
    const auto parsed=updates::ParseVersion(version.c_str());
    const auto kind=parsed?updates::ClassifyReleaseKind(*parsed):updates::ReleaseKind::Stable;
    const char* label="";
    for(const auto& info:updates::kUpdateChannels)if(info.kind==kind)label=info.labelKey;
    return loc::Tf("about.version_line",version,loc::T(label));
}
} }
