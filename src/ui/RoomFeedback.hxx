#pragma once
#include "ApplicationShell.hxx"

namespace sf4e { namespace ui {
inline bool RoomActionsAvailable(const ShellView& view) {
    return view.session.room == netplay::RoomState::Joined &&
        view.session.control == netplay::Health::Healthy &&
        view.session.recovery == netplay::Recovery::None && !view.room.closed &&
        (!view.session.coordinated || view.session.authorityWritable);
}

inline const char* RoomWaitReason(const ShellView& view) {
    if (view.session.room == netplay::RoomState::Closing)
        return "Leaving room. Waiting for the connection to close...";
    if (view.session.room == netplay::RoomState::Opening)
        return "Joining room. Waiting for the other players...";
    if (view.room.closed) return "This room has closed. Return to Online Play to join another.";
    if (view.session.control != netplay::Health::Healthy || view.session.recovery != netplay::Recovery::None)
        return "Room connection is recovering. You can still leave the room.";
    return "Updating room. Controls will return when the update finishes.";
}

struct ConnectionCheckFeedback {
    std::string value, detail, action;
    bool checking = false;
};
inline ConnectionCheckFeedback DescribeConnectionCheck(const ShellView& view) {
    ConnectionCheckFeedback result;
    result.checking = view.probeStatus == "checking";
    const bool measured = view.recommendedDelay >= 0 && view.recommendedDelay <= 10;
    if (result.checking) {
        result.value = "Checking...";
        result.detail = "Measuring your opponent's connection. This usually takes a few seconds. You can still choose a delay manually.";
        result.action = "Checking connection...";
    } else if (measured) {
        result.value = std::to_string(view.recommendedDelay) + " frames";
        result.detail = "Check complete: " + std::to_string(view.probeSamples) + " replies, " +
            std::to_string(view.probeLost) + " missed. Apply this recommendation or choose your own delay.";
        result.action = "Check connection again";
    } else if (!view.probeStatus.empty()) {
        result.value = "No recommendation";
        result.detail = view.probeStatus == "invalidated" ?
            "The connection or opponent changed. Run the check again, or choose a delay and Ready." :
            "The check could not collect enough replies. Retry, or choose a delay and Ready; a recommendation is optional.";
        result.action = "Retry connection check";
    } else {
        result.value = "Not checked";
        result.detail = "Check your opponent's connection for a suggested delay. This is optional; you can choose a delay and Ready.";
        result.action = "Check connection";
    }
    return result;
}
} }
