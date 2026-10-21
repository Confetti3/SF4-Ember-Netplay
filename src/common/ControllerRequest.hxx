#pragma once
#include "PadSetup.hxx"
#include "ButtonLayoutTypes.hxx"
namespace sf4e { namespace gamepads {
struct ControllerRequest {
    enum class Kind { None, Play, Setup, Forget, SetupControl, Prompt, DeclinePrompt, OpenLayout, Layout, RemoveLayout, LayoutLog } kind = Kind::None;
    unsigned id = 0;
    SetupCommand command = SetupCommand::None;
    int step = 0;
    ButtonLayout buttons = DefaultLayout;
    BoundButtons bound = NoBoundButtons;
    ExtraCopies extras = NoExtraCopies;
    std::string log;
};
} }
