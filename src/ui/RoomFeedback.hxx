#pragma once
#include "ApplicationShell.hxx"

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
        return "Leaving room. Waiting for the connection to close...";
    if (view.session.room == netplay::RoomState::Opening)
        return "Joining room. Waiting for the other players...";
    if (view.room.closed) return "This room has closed. Return to Online Play to join another.";
    if (view.session.control != netplay::Health::Healthy || view.session.recovery != netplay::Recovery::None)
        return "Room connection is recovering. You can still leave the room.";
    return "Updating room. Controls may pause briefly.";
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
        result.value = "Checking...";
        result.detail = view.probeBenchmark ?
            "Benchmarking gameplay-size datagrams for 30 seconds, plus connection setup. This measures network performance, not game FPS." :
            "Measuring gameplay datagrams for five seconds, plus connection setup. You can still choose a delay manually.";
        result.action = "Checking connection...";
    } else if (measured) {
        result.value = std::to_string(view.recommendedDelay) + " frames";
        result.detail = view.probeRoute + " connection. RTT median " + ProbeMilliseconds(view.probeP50Us) +
            " ms; p95 " + ProbeMilliseconds(view.probeP95Us) + " ms; p99 " + ProbeMilliseconds(view.probeP99Us) +
            " ms. RTT variation " + ProbeMilliseconds(view.probeJitterUs) + " ms. " +
            std::to_string(view.probeSent) + " sent, " + std::to_string(view.probeSamples) + " replies, " + std::to_string(view.probeLost) +
            " missed. Apply this recommendation or choose your own delay.";
        result.action = "Check connection again";
    } else if (!view.probeStatus.empty()) {
        result.value = "No recommendation";
        result.detail = view.probeStatus == "timed_out" ?
            "The connection check timed out. Retry, or choose a delay and Ready." : view.probeStatus == "invalidated" ?
            "The connection or opponent changed. Run the check again, or choose a delay and Ready." :
            view.probeStatus == "local_overload" ?
            "This PC could not send the full workload on time. Retry with less background load, or choose a delay manually." :
            "The check could not collect enough replies. Retry, or choose a delay and Ready; a recommendation is optional.";
        if(view.probeSent && view.probeStatus!="invalidated") result.detail += " Sent " + std::to_string(view.probeSent) +
            " of " + std::to_string(view.probeExpected) + " scheduled packets; " + std::to_string(view.probeSamples) +
            " replies, " + std::to_string(view.probeLost) + " missed.";
        result.action = "Retry connection check";
    } else {
        result.value = "Not checked";
        result.detail = "Check your opponent's connection for a suggested delay. This is optional; you can choose a delay and Ready.";
        result.action = "Check connection";
    }
    return result;
}
} }
