#include "../sf4e/sf4e__NetplayFacade.hxx"
#include "../sf4e/sf4e__InputDevices.hxx"
#include "../Dimps/Dimps__Pad.hxx"
#include <cstdlib>
#include <iostream>
#define CHECK(c) do { if (!(c)) { std::cerr << "Check failed at " << __LINE__ << ": " #c << '\n'; std::exit(1); } } while (false)
using Pad = Dimps::Pad::System;
using Backend = Dimps::Pad::System_XInput;
struct FakePad : Pad {
    int type[2] = {1,1}, index[2] = {0,0}, assigned[2] = {-1,-1}, mapping = -1;
    int Type(int side) { return type[side]; }
    int Index(int side) { return index[side]; }
    void Associate(int side, int value) { index[side] = value; }
    bool rejectBinding=false;
    void SetType(int side, int value) { if(!rejectBinding)type[side] = value; }
    unsigned Mapped(int side) { return 0x10u << side; }
    unsigned Raw(int side) { return 0x40000u << side; }
    void Assigned(int side, int value) { assigned[side] = value ? side : -1; }
    void Mapping(int value) { mapping = value; }
};
struct FakeBackend : Backend {
    bool connected[6] = {true,false,false,false,true,true};
    unsigned buttons[6] = {};
    unsigned released = 0;
    unsigned Release(int index, int inUse) { if (!inUse) released |= 1u << index; return 0; }
    int Count() { return 6; }
    int Status(int index) { return connected[index] ? 1 : 0; }
    char* Name(int index) { return const_cast<char*>(index == 0 ? "Xbox pad" : index == 4 ? "DirectInput stick" : "Leverless pad"); }
    unsigned Buttons(int index) { return buttons[index]; }
};
static FakePad pad;
static FakeBackend backend;
static Pad* Singleton() { return &pad; }
static Backend* Controllers() { return &backend; }
static Dimps::Pad::System_RawInput* NoRaw() { return nullptr; }
int main() {
    Pad::staticMethods.GetSingleton = Singleton;
    Dimps::Pad::System_RawInput::staticMethods.GetSingleton = NoRaw;
    auto& methods = Pad::publicMethods;
    methods.GetDeviceTypeForPlayer = static_cast<int(Pad::*)(int)>(&FakePad::Type);
    methods.GetDeviceIndexForPlayer = static_cast<int(Pad::*)(int)>(&FakePad::Index);
    methods.GetButtons_MappedOn = static_cast<unsigned(Pad::*)(int)>(&FakePad::Mapped);
    methods.GetButtons_RawOn = static_cast<unsigned(Pad::*)(int)>(&FakePad::Raw);
    methods.AssociatePlayerAndGamepad = static_cast<void(Pad::*)(int,int)>(&FakePad::Associate);
    methods.SetDeviceTypeForPlayer = static_cast<void(Pad::*)(int,int)>(&FakePad::SetType);
    methods.SetSideHasAssignedController = static_cast<void(Pad::*)(int,int)>(&FakePad::Assigned);
    methods.SetActiveButtonMapping = static_cast<void(Pad::*)(int)>(&FakePad::Mapping);
    Backend::staticMethods.GetSingleton = Controllers;
    Backend::publicMethods.SetDeviceInUse = static_cast<unsigned(Backend::*)(int,int)>(&FakeBackend::Release);
    Backend::publicMethods.GetDeviceCount = static_cast<int(Backend::*)()>(&FakeBackend::Count);
    Backend::publicMethods.GetDeviceStatus = static_cast<int(Backend::*)(int)>(&FakeBackend::Status);
    Backend::publicMethods.GetDeviceName = static_cast<char*(Backend::*)(int)>(&FakeBackend::Name);
    Backend::publicMethods.GetButtonsOn = static_cast<unsigned(Backend::*)(int)>(&FakeBackend::Buttons);
    const auto sample = [] {
        auto devices = sf4e::input::ReadDevices();
        devices.front().buttons = 0; // Keep physical test-runner keyboard activity out of the fixture.
        return devices;
    };
    backend.connected[4]=backend.connected[5]=false;
    CHECK(sf4e::input::MenuDevice(sample()).type==3);
    backend.connected[4]=backend.connected[5]=true;
    CHECK(!sf4e::input::MenuDevice(sample()).connected);
    pad.type[0]=4;pad.index[0]=5;CHECK(sf4e::input::MenuDevice(sample()).index==5);
    for (int index : {0,4,5}) {
        pad.type[0] = 1; pad.index[0] = 0;
        sf4e::input::Assignment assignment;
        assignment.Adopt(sample().front());
        CHECK(assignment.Selected().type == 1 && assignment.Ready());
        backend.buttons[index] = 64;
        assignment.Begin(); assignment.Tick(sample());
        CHECK(assignment.State() == sf4e::input::Capture::ReleaseAll);
        backend.buttons[index] = 0; assignment.Tick(sample());
        CHECK(assignment.State() == sf4e::input::Capture::Press);
        backend.buttons[index] = 64; assignment.Tick(sample());
        CHECK(!assignment.Ready() && assignment.Selected().type == 1);
        backend.buttons[index] = 0; CHECK(assignment.Tick(sample()));
        const int expectedType = index < 4 ? 3 : 4;
        CHECK(assignment.Selected().type == expectedType && assignment.Selected().index == index);
        for (int rematch = 0; rematch < 2; ++rematch) for (int side : {0,1}) {
            CHECK(sf4e::input::AssignToSide(assignment.Selected(), side, true));
            CHECK(pad.index[side] == index && pad.type[side] == expectedType && pad.assigned[side] == side);
            CHECK(pad.mapping == Pad::BUTTON_MAPPING_FIGHT);
            unsigned mapped=0,raw=0;
            CHECK(sf4e::input::ReadAssignedInput(assignment.Selected(),side,mapped,raw));
            CHECK(mapped==(0x10u<<side)&&raw==(0x40000u<<side));
            pad.index[side]=(index+1)%6;
            CHECK(!sf4e::input::ReadAssignedInput(assignment.Selected(),side,mapped,raw)&&!mapped&&!raw);
            pad.index[side]=index;
            if (side == 1) CHECK(pad.assigned[0] == -1 && (backend.released & (1u << index)));
        }
        assignment.Begin(); assignment.Cancel(); CHECK(assignment.Ready());
        const auto stale=assignment.Selected();backend.connected[index] = false;
        CHECK(!sf4e::input::AssignToSide(stale,0,true));
        unsigned mapped=1,raw=1;CHECK(!sf4e::input::ReadAssignedInput(stale,1,mapped,raw)&&!mapped&&!raw);
        assignment.Tick(sample()); CHECK(!assignment.Ready());
        CHECK(!sf4e::input::AssignToSide(assignment.Selected(), 0, true));
        backend.connected[index] = true; assignment.Tick(sample()); CHECK(assignment.Ready());
        assignment.Begin(); assignment.Tick(sample()); backend.buttons[index] = 64; assignment.Tick(sample());
        backend.connected[index] = false; assignment.Tick(sample());
        CHECK(assignment.State() == sf4e::input::Capture::ReleaseAll);
        assignment.Cancel(); backend.buttons[index] = 0; backend.connected[index] = true;
        pad.type[0]=1;pad.rejectBinding=true;CHECK(!sf4e::input::AssignToSide(assignment.Selected(),0,true));pad.rejectBinding=false;
    }
    sf4e::input::Assignment ambiguous;ambiguous.Begin();ambiguous.Tick(sample());
    backend.buttons[0]=backend.buttons[4]=0x200;ambiguous.Tick(sample());
    CHECK(ambiguous.State()==sf4e::input::Capture::ReleaseAll&&!ambiguous.Ready());
    backend.buttons[0]=backend.buttons[4]=0;ambiguous.Tick(sample());
    backend.buttons[4]=0x200;ambiguous.Tick(sample());backend.buttons[4]=0;
    CHECK(ambiguous.Tick(sample())&&ambiguous.Selected().index==4);
    backend.connected[4]=false;ambiguous.Tick(sample());CHECK(!ambiguous.Ready()&&ambiguous.Selected().index==4);
    std::cout << "Native adapter: stale keyboard replaced by XInput, DirectInput and leverless devices; both slots, held buttons, cancel, disconnect and rematches passed\n";
}
