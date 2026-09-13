#pragma once
#include <string>
namespace sf4e { namespace netplay {
enum class MemberRole { Player, Spectator };
struct MemberView {
    std::string name;
    MemberRole role = MemberRole::Player;
    bool ready = false;
    MemberView() = default;
    MemberView(const char* value) : name(value) {}
    MemberView(const std::string& value) : name(value) {}
};
enum class NetworkAvailability { Starting, Ready, Unavailable };
} }
