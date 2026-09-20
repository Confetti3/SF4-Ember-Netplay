#pragma once
#include "ApplicationShell.hxx"
#include "../common/Localization.hxx"

namespace sf4e { namespace ui {
inline bool RoomActionsAvailable(const ShellView& view) {
    return view.session.room == netplay::RoomState::Joined &&
        view.session.control == netplay::Health::Healthy &&
        view.session.recovery == netplay::Recovery::None && !view.room.closed &&
        (!view.session.coordinated || view.session.authorityWritable);
}

// A healthy control revision may precede application of its checkpoint.
// Commands remain fenced, but committed match feedback is still valid.
inline bool RoomCheckpointPending(const ShellView& view) {
    return view.session.room == netplay::RoomState::Joined &&
        view.session.control == netplay::Health::Healthy &&
        view.session.recovery == netplay::Recovery::None && !view.room.closed &&
        view.session.coordinated && !view.session.authorityWritable;
}

inline const char* RoomWaitReason(const ShellView& view) {
    if (view.session.room == netplay::RoomState::Closing)
        return loc::T("room.wait.leaving");
    if (view.session.room == netplay::RoomState::Opening)
        return loc::T("room.wait.joining");
    if (view.room.closed) return loc::T("room.wait.closed");
    if (view.session.control != netplay::Health::Healthy || view.session.recovery != netplay::Recovery::None)
        return loc::T("room.wait.recovering");
    return loc::T("room.wait.updating");
}

struct ConnectionCheckFeedback {
    std::string value, detail, action;
    bool checking = false;
};
inline std::string ProbeMilliseconds(std::uint64_t us) {
    return std::to_string(us/1000)+"."+std::to_string((us%1000)/100);
}
inline ConnectionCheckFeedback DescribeConnectionCheck(const ShellView& view) {
    ConnectionCheckFeedback result;
    result.checking = view.probeStatus == "checking";
    const bool measured = view.recommendedDelay >= 0 && view.recommendedDelay <= 10;
    if (result.checking) {
        result.value = loc::T("connection.checking");
        result.detail = loc::T("connection.checking_detail");
        result.action = loc::T("connection.checking_action");
    } else if (measured) {
        result.value = loc::Tf("connection.frames",view.recommendedDelay);
        result.detail = loc::Tf("connection.result_detail",view.probeRoute,ProbeMilliseconds(view.probeP50Us),
            ProbeMilliseconds(view.probeP95Us),ProbeMilliseconds(view.probeP99Us),ProbeMilliseconds(view.probeJitterUs),
            view.probeSent,view.probeSamples,view.probeLost);
        result.action = loc::T("connection.check_again");
    } else if (!view.probeStatus.empty()) {
        result.value = loc::T("connection.no_recommendation");
        result.detail = view.probeStatus == "timed_out" ?
            loc::T("connection.timed_out") : view.probeStatus == "invalidated" ?
            loc::T("connection.invalidated") :
            view.probeStatus == "local_overload" ?
            loc::T("connection.local_overload") : loc::T("connection.insufficient");
        if(view.probeSent && view.probeStatus!="invalidated") result.detail += loc::Tf("connection.packet_summary",view.probeSent,view.probeExpected,view.probeSamples,view.probeLost);
        result.action = loc::T("connection.retry");
    } else {
        result.value = loc::T("connection.not_checked");
        result.detail = loc::T("connection.not_checked_detail");
        result.action = loc::T("connection.check");
    }
    return result;
}
} }
