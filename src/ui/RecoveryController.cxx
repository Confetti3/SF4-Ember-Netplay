#include "RecoveryController.hxx"
#include "MenuNavigation.hxx"
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <cstring>
#include <cwchar>

namespace sf4e { namespace ui {
namespace {
constexpr unsigned Up=MenuInput::Up, Down=MenuInput::Down, Left=MenuInput::Left, Right=MenuInput::Right;
constexpr unsigned Select=MenuInput::Select, Back=MenuInput::Back;
unsigned Buttons(bool select,bool back) { return (select?Select:0)|(back?Back:0); }
}
unsigned StickDirections(int x,int y,unsigned previous) {
    const auto axis=[&](int v,unsigned negative,unsigned positive) {
        const auto limit=[&](unsigned bit){return previous&bit?RecoveryControllerMap::Release:RecoveryControllerMap::Press;};
        return v<=-limit(negative)?negative:v>=limit(positive)?positive:0u;
    };
    return axis(x,Left,Right)|axis(y,Up,Down);
}
unsigned PovDirections(std::uint32_t pov) {
    if((pov&0xFFFFu)==0xFFFFu) return 0;
    // Eight sectors of 45 degrees, so a diagonal holds both of its directions.
    const unsigned a=pov%36000u; unsigned bits=0;
    if(a>29250||a<6750) bits|=Up;
    if(a>2250&&a<15750) bits|=Right;
    if(a>11250&&a<24750) bits|=Down;
    if(a>20250&&a<33750) bits|=Left;
    return bits;
}
unsigned RecoveryControllerMap::Admit(Part& part,unsigned raw,bool fresh) {
    part.blocked=fresh?raw:part.blocked&raw; part.raw=raw;
    return raw&~part.blocked;
}
unsigned RecoveryControllerMap::Update(const RecoveryPadSample& sample) {
    unsigned pads=0, sticks=0; bool anyPad=false;
    for(std::size_t i=0;i<sample.pads.size();++i) {
        const auto& pad=sample.pads[i];
        if(!pad.connected) { padsSeen_[i]=false; continue; }
        const bool fresh=!padsSeen_[i]; padsSeen_[i]=anyPad=true;
        if(fresh) pads_[i]=Device{};
        auto& parts=pads_[i].parts;
        // XInput's thumb y grows upward; the shared helper expects DirectInput's downward y.
        const int x=pad.thumbX*1000/32767, y=-pad.thumbY*1000/32767;
        // The XInput d-pad bits are the menu's own Up/Down/Left/Right.
        pads|=Admit(parts[0],pad.buttons&(Up|Down|Left|Right),fresh);
        pads|=Admit(parts[1],StickDirections(x,y,parts[1].raw),fresh);
        pads|=Admit(parts[2],Buttons((pad.buttons&XINPUT_GAMEPAD_A)!=0,(pad.buttons&XINPUT_GAMEPAD_B)!=0),fresh);
    }
    std::map<std::uint64_t,Device> seen;
    for(const auto& stick:sample.sticks) {
        const auto old=sticks_.find(stick.id); const bool fresh=old==sticks_.end();
        auto device=fresh?Device{}:old->second;
        auto& parts=device.parts;
        sticks|=Admit(parts[0],PovDirections(stick.pov),fresh);
        sticks|=Admit(parts[1],stick.axes?StickDirections(stick.x,stick.y,parts[1].raw):0u,fresh);
        sticks|=Admit(parts[2],Buttons((stick.buttons&1u)!=0,(stick.buttons&2u)!=0),fresh);
        seen[stick.id]=device;
    }
    sticks_=std::move(seen);
    const unsigned padPressed=pads&~previousPads_, stickPressed=sticks&~previousSticks_, keyPressed=sample.keyboard&~previousKeyboard_;
    previousPads_=pads; previousSticks_=sticks; previousKeyboard_=sample.keyboard;
    if(keyPressed) { family_=PadFamily::Keyboard; used_=true; }
    if(stickPressed) { family_=PadFamily::DirectInput; used_=true; }
    if(padPressed) { family_=PadFamily::Xbox; used_=true; }
    // Until something is pressed, or once the device last used is gone, name
    // the controller most likely to be picked up.
    const bool gone=(family_==PadFamily::Xbox&&!anyPad)||(family_==PadFamily::DirectInput&&sample.sticks.empty());
    if(!used_||gone) family_=anyPad?PadFamily::Xbox:!sample.sticks.empty()?PadFamily::DirectInput:PadFamily::Keyboard;
    // The sticks never include an XInput pad (the poller leaves those out of
    // DirectInput), so each device is counted once. Opposite directions from
    // two devices cancel in MenuNavigation::Update.
    return pads|sticks;
}
unsigned RecoveryControllerMap::Focus(unsigned held,bool active) {
    // Like ControllerNavigation: after focus returns, nothing counts until
    // every pad is released, so a button held in another window cannot press.
    if(!active) { rearmed_=false; return 0; }
    if(!rearmed_) { rearmed_=held==0; return 0; }
    return held;
}

struct RecoveryController::Hardware {
    using GetStateFn=DWORD(WINAPI*)(DWORD,XINPUT_STATE*);
    struct Stick {
        GUID instance{}; IDirectInputDevice8W* device=nullptr; std::uint64_t id=0;
        bool axes=false, present=false; LONG range[4]{};
    };
    HWND window;
    HMODULE xinput=nullptr; GetStateFn getState=nullptr;
    // Product ids (VID in the low word, PID in the high) of the XInput pads,
    // which DirectInput also lists; they are read through XInput only.
    std::vector<DWORD> xinputProducts;
    std::array<bool,4> connected{}; ULONGLONG nextProbe=0;
    IDirectInput8W* input=nullptr;
    std::vector<Stick> sticks; ULONGLONG nextEnumerate=0; bool changed=true;

    explicit Hardware(HWND owner) : window(owner) {
        // Loaded dynamically like the ImGui backend, so a system without the
        // newest runtime still starts; 9_1_0 ships with every Windows since Vista.
        for(const auto* name:{L"xinput1_4.dll",L"xinput1_3.dll",L"xinput9_1_0.dll",L"xinput1_2.dll",L"xinput1_1.dll"})
            if((xinput=LoadLibraryW(name))!=nullptr) break;
        if(xinput) getState=reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(xinput,"XInputGetState")));
        if(FAILED(DirectInput8Create(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,IID_IDirectInput8W,
            reinterpret_cast<void**>(&input),nullptr))) input=nullptr;
    }
    ~Hardware() {
        for(auto& s:sticks) Drop(s);
        if(input) input->Release();
        if(xinput) FreeLibrary(xinput);
    }
    static void Drop(Stick& s) { s.device->Unacquire(); s.device->Release(); s.device=nullptr; }
    static bool Range(IDirectInputDevice8W* device,DWORD offset,LONG* range) {
        DIPROPRANGE r{}; r.diph.dwSize=sizeof(r); r.diph.dwHeaderSize=sizeof(DIPROPHEADER);
        r.diph.dwObj=offset; r.diph.dwHow=DIPH_BYOFFSET; r.lMin=-1000; r.lMax=1000;
        // Some drivers refuse a new range; read back whichever one applies.
        device->SetProperty(DIPROP_RANGE,&r.diph);
        if(FAILED(device->GetProperty(DIPROP_RANGE,&r.diph))||r.lMax<=r.lMin) return false;
        range[0]=r.lMin; range[1]=r.lMax; return true;
    }
    static int Normalize(LONG v,const LONG* range) {
        return static_cast<int>((static_cast<long long>(v)-range[0])*2000/(static_cast<long long>(range[1])-range[0])-1000);
    }
    static BOOL CALLBACK Found(LPCDIDEVICEINSTANCEW found,LPVOID self) {
        static_cast<Hardware*>(self)->Open(*found); return DIENUM_CONTINUE;
    }
    // Microsoft's documented test: an XInput device's raw input name holds "IG_".
    void FindXInputProducts() {
        xinputProducts.clear();UINT count=0;
        if(GetRawInputDeviceList(nullptr,&count,sizeof(RAWINPUTDEVICELIST))!=0||!count) return;
        std::vector<RAWINPUTDEVICELIST> list(count);
        if(GetRawInputDeviceList(list.data(),&count,sizeof(RAWINPUTDEVICELIST))==static_cast<UINT>(-1)) return;
        for(UINT i=0;i<count;++i) {
            if(list[i].dwType!=RIM_TYPEHID) continue;
            wchar_t name[512]={};UINT size=512;
            if(GetRawInputDeviceInfoW(list[i].hDevice,RIDI_DEVICENAME,name,&size)==static_cast<UINT>(-1)||!std::wcsstr(name,L"IG_")) continue;
            RID_DEVICE_INFO info{};info.cbSize=sizeof(info);size=sizeof(info);
            if(GetRawInputDeviceInfoW(list[i].hDevice,RIDI_DEVICEINFO,&info,&size)==static_cast<UINT>(-1)) continue;
            xinputProducts.push_back(MAKELONG(info.hid.dwVendorId,info.hid.dwProductId));
        }
    }
    void Open(const DIDEVICEINSTANCEW& found) {
        for(const auto product:xinputProducts) if(found.guidProduct.Data1==product) return;
        for(auto& s:sticks) if(IsEqualGUID(s.instance,found.guidInstance)) { s.present=true; return; }
        IDirectInputDevice8W* device=nullptr;
        if(FAILED(input->CreateDevice(found.guidInstance,&device,nullptr))) return;
        // Background and non-exclusive: the recovery window never takes the
        // stick from another program, and focus changes do not unacquire it.
        if(FAILED(device->SetDataFormat(&c_dfDIJoystick2))||
           FAILED(device->SetCooperativeLevel(window,DISCL_BACKGROUND|DISCL_NONEXCLUSIVE))) { device->Release(); return; }
        Stick s; s.instance=found.guidInstance; s.device=device; s.present=true;
        std::uint64_t parts[2]; std::memcpy(parts,&found.guidInstance,sizeof(parts)); s.id=parts[0]^parts[1];
        s.axes=Range(device,DIJOFS_X,s.range)&&Range(device,DIJOFS_Y,s.range+2);
        device->Acquire();
        sticks.push_back(s);
    }
    void Enumerate() {
        for(auto& s:sticks) s.present=false;
        FindXInputProducts();
        input->EnumDevices(DI8DEVCLASS_GAMECTRL,&Hardware::Found,this,DIEDFL_ATTACHEDONLY);
        for(auto it=sticks.begin();it!=sticks.end();) if(it->present) ++it; else { Drop(*it); it=sticks.erase(it); }
    }
    RecoveryPadSample Sample() {
        RecoveryPadSample sample; const auto now=GetTickCount64();
        if(getState) {
            // XInputGetState on an empty slot is slow on some systems, so empty
            // slots are probed once a second or after a device change.
            const bool probe=changed||now>=nextProbe; if(probe) nextProbe=now+1000;
            for(DWORD i=0;i<4;++i) {
                if(!connected[i]&&!probe) continue;
                XINPUT_STATE state{}; connected[i]=getState(i,&state)==ERROR_SUCCESS;
                if(!connected[i]) continue;
                auto& pad=sample.pads[i]; pad.connected=true; pad.buttons=state.Gamepad.wButtons;
                pad.thumbX=state.Gamepad.sThumbLX; pad.thumbY=state.Gamepad.sThumbLY;
            }
        }
        if(input) {
            // WM_DEVICECHANGE can arrive before a new stick is ready, so a
            // periodic pass also catches hot-plugs the message missed.
            if(changed||now>=nextEnumerate) { Enumerate(); nextEnumerate=now+2000; }
            for(auto& s:sticks) {
                DIJOYSTATE2 state{};
                if(FAILED(s.device->Poll())) s.device->Acquire();
                HRESULT result=s.device->GetDeviceState(sizeof(state),&state);
                if(result==DIERR_INPUTLOST||result==DIERR_NOTACQUIRED)
                    result=SUCCEEDED(s.device->Acquire())?s.device->GetDeviceState(sizeof(state),&state):result;
                // Left out of the sample, the stick counts as detached, so on
                // return anything it already holds waits for release.
                if(FAILED(result)) continue;
                DirectInputStickSample stick; stick.id=s.id; stick.pov=state.rgdwPOV[0]; stick.axes=s.axes;
                if(s.axes) { stick.x=Normalize(state.lX,s.range); stick.y=Normalize(state.lY,s.range+2); }
                for(unsigned b=0;b<32;++b) if(state.rgbButtons[b]&0x80) stick.buttons|=1u<<b;
                sample.sticks.push_back(stick);
            }
        }
        changed=false;
        return sample;
    }
};
RecoveryController::RecoveryController(HWND__* window) : hardware_(std::make_unique<Hardware>(window)) {}
RecoveryController::~RecoveryController()=default;
void RecoveryController::DevicesChanged() { hardware_->changed=true; }
unsigned RecoveryController::Poll(unsigned keyboard,bool active) {
    auto sample=hardware_->Sample(); sample.keyboard=keyboard;
    return map_.Focus(map_.Update(sample),active);
}
} }
