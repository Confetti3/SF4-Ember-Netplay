#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sf4e { namespace input {
enum class Action { None, BeginCapture, UseKeyboard, Cancel };
enum class Capture { Idle, ReleaseAll, Press, ReleaseSelected };
struct Device {
    int type = -1, index = -1;
    std::string name;
    bool connected = false;
    unsigned buttons = 0;
    bool Same(const Device& other) const { return type == other.type && index == other.index && name == other.name; }
};
class Assignment {
    Device selected_, candidate_;
    Capture capture_ = Capture::Idle;
public:
    const Device& Selected() const { return selected_; }
    Capture State() const { return capture_; }
    void Adopt(const Device& device) { selected_ = device; }
    void Begin() { capture_ = Capture::ReleaseAll; }
    void Cancel() { capture_ = Capture::Idle; candidate_ = {}; }
    bool Ready() const { return capture_ == Capture::Idle && selected_.connected; }
    // Sampled through the game's device backend, independent of ImGui navigation.
    bool Tick(const std::vector<Device>& devices) {
        selected_.connected = false;
        for (const auto& device : devices) if (selected_.Same(device)) selected_ = device;
        if (capture_ == Capture::Idle) return false;
        if (capture_ == Capture::ReleaseAll) {
            for (const auto& device : devices) if (device.connected && device.buttons) return false;
            capture_ = Capture::Press;
            return false;
        }
        if (capture_ == Capture::Press) {
            // Keyboard has an explicit action; controller capture never silently chooses it.
            const Device* pressed=nullptr;
            for (const auto& device : devices) if (device.type != 1 && device.connected && device.buttons) {
                if(pressed){capture_=Capture::ReleaseAll;candidate_={};return false;}
                pressed=&device;
            }
            if(pressed){candidate_=*pressed;capture_=Capture::ReleaseSelected;}
            return false;
        }
        for (const auto& device : devices) if (candidate_.Same(device) && device.connected) {
            if (device.buttons) return false;
            selected_ = device; Cancel(); return true;
        }
        capture_ = Capture::ReleaseAll;
        return false;
    }
};
} }
