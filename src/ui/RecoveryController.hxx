#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

struct HWND__;
namespace sf4e { namespace ui {
// Controller input for the launcher's recovery window, which runs before the
// game and so has none of its pad handling. The ImGui backend is built without
// gamepad support and would read only XInput slot 0 anyway.
enum class PadFamily { Keyboard, Xbox, DirectInput };
struct XInputPadSample { bool connected=false; std::uint16_t buttons=0; std::int16_t thumbX=0, thumbY=0; };
struct DirectInputStickSample {
    std::uint64_t id=0;             // stable for one attached device instance
    std::uint32_t pov=0xFFFFFFFFu;  // hundredths of a degree clockwise from up; centred when the low word is 0xFFFF
    bool axes=false; int x=0, y=0;  // normalised to [-1000,1000], y grows downward as in DirectInput
    std::uint32_t buttons=0;        // bit i is button i+1
};
struct RecoveryPadSample {
    std::array<XInputPadSample,4> pads{};
    std::vector<DirectInputStickSample> sticks;
    unsigned keyboard=0;            // MenuInput bits the keyboard holds; steers glyphs only
};
// Pure mapping from device samples to MenuInput::held bits, testable without
// hardware. Keyboard bits are not returned: ReadMenuInput adds the keyboard.
class RecoveryControllerMap {
public:
    // A stick moves a direction past Press and releases it inside Release, so
    // an axis resting near the threshold cannot chatter into repeated moves.
    static constexpr int Press=500, Release=350;
    unsigned Update(const RecoveryPadSample& sample);
    // Held bits while the window is in front; after it comes back, none until
    // every pad has been released.
    unsigned Focus(unsigned held,bool active);
    // The window lost focus, was minimized or opened a native dialog: nothing
    // counts again until every pad is released, whether or not a poll ran.
    void Deactivate() { rearmed_=false; }
    PadFamily Family() const { return family_; }
private:
    // A device reports three parts (d-pad or hat, stick, buttons). A part that
    // is already active when its device appears is ignored until released, so
    // a stick whose axis rests off centre, or a button held through a
    // hot-plug, can neither hold a direction nor keep the neutral gate shut.
    struct Part { unsigned raw=0, blocked=0; };
    struct Device { std::array<Part,3> parts; };
    static unsigned Admit(Part& part,unsigned raw,bool fresh);
    std::array<Device,4> pads_{};
    std::array<bool,4> padsSeen_{};
    std::map<std::uint64_t,Device> sticks_;
    unsigned previousPads_=0, previousSticks_=0, previousKeyboard_=0;
    bool used_=false, rearmed_=true;
    PadFamily family_=PadFamily::Keyboard;
};
// Stick directions shared by both paths; y grows downward.
unsigned StickDirections(int x,int y,unsigned previous);
unsigned PovDirections(std::uint32_t pov);

// Polls XInput slots 0-3 and every attached DirectInput game controller that
// is not an XInput pad.
class RecoveryController {
public:
    explicit RecoveryController(HWND__* window);
    ~RecoveryController();
    RecoveryController(const RecoveryController&)=delete;
    RecoveryController& operator=(const RecoveryController&)=delete;
    // WM_DEVICECHANGE: look for new or removed devices on the next poll.
    void DevicesChanged();
    void Deactivate() { map_.Deactivate(); }
    // Menu bits from every pad. `active` false (window in the background)
    // still polls, so blocked parts stay tracked, but returns nothing, and on
    // return nothing counts until everything is released.
    unsigned Poll(unsigned keyboard,bool active);
    PadFamily Family() const { return map_.Family(); }
private:
    struct Hardware;
    std::unique_ptr<Hardware> hardware_;
    RecoveryControllerMap map_;
};
} }
