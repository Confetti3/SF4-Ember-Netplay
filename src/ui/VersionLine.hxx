#pragma once
#include "../common/Localization.hxx"
#include "../common/UpdateChannel.hxx"
#include <string>

namespace sf4e { namespace ui {
// "Ember 1.2.0 · Nightly": the version on this PC and the channel its updates
// come from. Home's corner, the launcher's window and Help and about all show
// this one line. Empty when there is no version to show.
inline std::string VersionLine(const std::string& version,updates::UpdateChannel channel) {
    if(version.empty())return {};
    return loc::Tf("about.version_line",version,loc::T(updates::GetUpdateChannelInfo(channel).labelKey));
}
} }
