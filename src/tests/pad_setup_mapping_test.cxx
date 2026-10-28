#include "../common/GamepadBrand.hxx"
#include "../common/PadSetup.hxx"
#include "../netplay/ControllerMappings.hxx"
#include "../sf4e/sf4e__GamepadBindings.hxx"
#include <SDL3/SDL.h>
#include <array>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "test_support.hxx"
using namespace sf4e::gamepads;
int Step(const char* name) {
    for (int i = 0; i < SetupStepCount; ++i) if (std::string(SetupSteps[i].name) == name) return i;
    throw std::runtime_error(name);
}
SetupState Record(std::initializer_list<std::pair<const char*, RawInput>> steps) {
    SetupState state;
    for (const auto& step : steps) state.recorded[Step(step.first)] = step.second;
    return state;
}
std::size_t Count(const std::string& text, const std::string& part) {
    std::size_t count = 0;
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++count;
    return count;
}
constexpr int Lo = SdlAxisMin, Hi = SdlAxisMax;
SdlInput B(int button) { return {SdlBindType::Button, button}; }
SdlInput H(int hat, int mask) { return {SdlBindType::Hat, hat, 0, 0, mask}; }
SdlInput A(int axis, int min = Lo, int max = Hi) { return {SdlBindType::Axis, axis, min, max}; }
SdlBinding Bind(const char* target, SdlInput input) { return {input, SdlTarget(target)}; }
using Bindings = std::vector<SdlBinding>;
// SDL_FixupHIDAPIMapping's test: the first binding to a button from dpup on
// reads that same button number.
bool LegacyHidapi(const Bindings& bindings) {
    for (const auto& b : bindings) if (b.output.type == SdlBindType::Button && b.output.index >= SdlDpadUp)
        return b.input.type == SdlBindType::Button && b.input.index == b.output.index;
    return false;
}
std::string MappingText(const std::string& guid) {
    char* text = SDL_GetGamepadMappingForGUID(SDL_StringToGUID(guid.c_str()));
    std::string copy = text ? text : ""; SDL_free(text);
    return copy;
}
SDL_JoystickID Attach(const char* name, Uint16 vendor = 0, Uint16 product = 0) {
    SDL_VirtualJoystickDesc desc; SDL_INIT_INTERFACE(&desc);
    desc.naxes = 6; desc.nbuttons = 32; desc.nhats = 2; desc.name = name; desc.vendor_id = vendor; desc.product_id = product;
    const auto pad = SDL_AttachVirtualJoystick(&desc); CHECK_OR_THROW(pad);
    return pad;
}
std::string GuidOf(SDL_JoystickID pad) {
    char text[33]; SDL_GUIDToString(SDL_GetJoystickGUIDForID(pad), text, sizeof text);
    return text;
}

void Overlap() {
    // One button, hat masks that share a bit, and axis ranges on one side of 0.
    CHECK_OR_THROW(Overlaps(B(3), B(3)) && !Overlaps(B(3), B(4)) && !Overlaps(B(0), A(0)));
    CHECK_OR_THROW(Overlaps(H(0, 3), H(0, 1)) && Overlaps(H(0, 3), H(0, 2)) && !Overlaps(H(0, 3), H(0, 4)) && !Overlaps(H(0, 1), H(1, 1)));
    CHECK_OR_THROW(Overlaps(H(0, 17), H(0, 1)) && Overlaps(A(2), A(2, 0, Hi)) && Overlaps(A(2, Hi, Lo), A(2, 0, Lo)) && Overlaps(A(2, 0, Hi), A(2, Hi, 0)));
    CHECK_OR_THROW(!Overlaps(A(2, 0, Hi), A(2, 0, Lo)) && !Overlaps(A(2, Lo, 0), A(2, 0, Hi)) && !Overlaps(A(2), A(3)));
}
void PureMerge() {
    const auto lk = Record({{"lk", {RawKind::Button, 3}}});
    // A recorded target is replaced; an unrelated binding stays after the recorded ones.
    CHECK_OR_THROW(MergeSetup(lk, {Bind("a", B(0)), Bind("guide", B(10))}) == Bindings({Bind("a", B(3)), Bind("guide", B(10))}));
    // A reused input leaves its old target: a button, a composite hat, an axis half and a full axis.
    CHECK_OR_THROW(MergeSetup(lk, {Bind("y", B(3)), Bind("back", B(6))}) == Bindings({Bind("a", B(3)), Bind("back", B(6))}));
    const auto upRight = Record({{"up", {RawKind::HatUp, 0}}, {"right", {RawKind::HatRight, 0}}});
    CHECK_OR_THROW(MergeSetup(upRight, {Bind("leftshoulder", H(0, 3)), Bind("rightshoulder", H(0, 4)), Bind("lefttrigger", H(0, 6))}) ==
        Bindings({Bind("dpup", H(0, 1)), Bind("dpright", H(0, 2)), Bind("rightshoulder", H(0, 4))}));
    const auto stickLeft = Record({{"left", {RawKind::AxisMinus, 0}}});
    CHECK_OR_THROW(MergeSetup(stickLeft, {Bind("leftx", A(0)), Bind("start", A(0, 0, Hi)), Bind("lefty", A(1))}) ==
        Bindings({Bind("dpleft", A(0, 0, Lo)), Bind("start", A(0, 0, Hi)), Bind("lefty", A(1))}));
    const auto stickX = Record({{"lx", {RawKind::AxisPlus, 0}}});
    CHECK_OR_THROW(MergeSetup(stickX, {Bind("dpleft", A(0, 0, Lo)), Bind("dpright", A(0, Hi, 0)), Bind("lefty", A(1))}) == Bindings({Bind("leftx", A(0)), Bind("lefty", A(1))}));
    // A stick step takes its whole target: +rightx from a button replaces rightx.
    auto plusRightX = Bind("rightx", B(12)); plusRightX.output.min = 0;
    CHECK_OR_THROW(MergeSetup(Record({{"rx", {RawKind::Button, 12}}}), {Bind("rightx", A(3)), Bind("righty", A(4))}) == Bindings({plusRightX, Bind("righty", A(4))}));
    // A skipped step keeps the pad's own binding.
    auto skipped = lk; skipped.skipped[Step("3p")] = true;
    CHECK_OR_THROW(MergeSetup(skipped, {Bind("leftshoulder", B(4))}) == Bindings({Bind("a", B(3)), Bind("leftshoulder", B(4))}));
    // An unmapped pad: exactly the recorded steps.
    CHECK_OR_THROW(MergeSetup(lk, {}) == Bindings({Bind("a", B(3))}));
}
void Writing() {
    // An unmapped pad: the recorded pairs in step order, its face, SDL's positional mark, then platform.
    const std::string guid = "03000000790000001100000000000000", tail = "face:abxy,hint:!SDL_GAMECONTROLLER_USE_GAMECUBE_LABELS:=1,platform:Windows,";
    const auto write = [&](const std::string& name, const SetupState& state) { return SetupMapping(state, SdlSnapshot{}, guid, name, {}).value(); };
    auto state = Record({{"up", {RawKind::HatUp, 0}}, {"down", {RawKind::HatDown, 0}}, {"left", {RawKind::AxisMinus, 1}}, {"right", {RawKind::AxisPlus, 1}},
        {"lp", {RawKind::Button, 0}}, {"mp", {RawKind::Button, 1}}, {"hp", {RawKind::Button, 2}}, {"lk", {RawKind::Button, 3}}, {"mk", {RawKind::Button, 4}},
        {"hk", {RawKind::Trigger, 5, false, true}}, {"3p", {RawKind::Button, 6}}, {"3k", {RawKind::Trigger, 6}}, {"start", {RawKind::Button, 8}}, {"select", {RawKind::Button, 9}}});
    CHECK_OR_THROW(write("Full pad", state) == guid + ",Full pad,dpup:h0.1,dpdown:h0.4,dpleft:-a1,dpright:+a1,x:b0,y:b1,rightshoulder:b2,a:b3,b:b4,righttrigger:a5~,"
        "leftshoulder:b6,lefttrigger:a6,start:b8,back:b9," + tail);
    state.recorded[Step("3p")].reset(); state.recorded[Step("3k")].reset(); state.recorded[Step("select")].reset();
    state.recorded[Step("hk")] = RawInput{RawKind::Button, 5};
    CHECK_OR_THROW(write("Six button stick", state) == guid + ",Six button stick,dpup:h0.1,dpdown:h0.4,dpleft:-a1,dpright:+a1,x:b0,y:b1,rightshoulder:b2,a:b3,b:b4,righttrigger:b5,start:b8," + tail);
    CHECK_OR_THROW(write("Driver,name", state).find(",Driver name,") != std::string::npos);
    // Stick steps map the full axis; a button or hat drives the plus half.
    state.recorded[Step("ls")] = RawInput{RawKind::Button, 11}; state.recorded[Step("lx")] = RawInput{RawKind::AxisPlus, 0};
    state.recorded[Step("ly")] = RawInput{RawKind::AxisMinus, 2}; state.recorded[Step("rx")] = RawInput{RawKind::Button, 7};
    state.recorded[Step("ry")] = RawInput{RawKind::HatDown, 1};
    CHECK_OR_THROW(write("Stick pad", state) == guid + ",Stick pad,dpup:h0.1,dpdown:h0.4,dpleft:-a1,dpright:+a1,x:b0,y:b1,rightshoulder:b2,a:b3,b:b4,righttrigger:b5,"
        "leftstick:b11,leftx:a0,lefty:a2~,+rightx:b7,+righty:h1.4,start:b8," + tail);
    // Every input and output range in its canonical form.
    auto half = Bind("leftx", A(0, Hi, 0)); half.output = {SdlBindType::Axis, 0, 0, Lo};
    CHECK_OR_THROW(WriteSdlMapping(guid, "Forms", {{half, Bind("lefty", A(1, Lo, 0)), Bind("lefttrigger", A(2, Hi, Lo)), Bind("misc1", H(1, 17))}}).value() ==
        guid + ",Forms,-leftx:+a0~,lefty:-a1~,lefttrigger:a2~,misc1:h1.17," + tail);

    // Metadata as SDL reads it: the first of each key after the GUID and name,
    // plain values only. Face, type, hints and platform go; platform is written once, last.
    CHECK_OR_THROW(SdlMetadata(guid + ",Name type:ps4,a:b0,type:xbox360,face:abxy,sdk>=:21,sdk<=:99,crc:b003,hint:SDL_X:=1,type:ps5,platform:Windows,") ==
        "crc:b003,sdk>=:21,sdk<=:99,");
    CHECK_OR_THROW(SdlMetadata(guid + ",Name,a:b0,crc:67fa,platform:Windows,hint:!SDL_GAMECONTROLLER_USE_BUTTON_LABELS:=1, type : xbox360,Platform:Windows,") == "crc:67fa,");
    CHECK_OR_THROW(SdlMetadata(guid + ",Name,xtype:x-1,type:ps4,face:") == "" && SdlMetadata("") == "" && SdlMetadata(guid + ",Name") == "");
    // The snapshot's face and type are the only ones, ahead of the other metadata.
    const auto kept = WriteSdlMapping(guid, "Kept", {{Bind("a", B(3))}, SdlFace::Bayx, "switchpro"},
        guid + ",Kept,a:b0,face:abxy,crc:67fa,hint:SDL_GAMECONTROLLER_USE_BUTTON_LABELS:=1,type:xbox360,platform:Windows,").value();
    CHECK_OR_THROW(kept == guid + ",Kept,a:b3,face:bayx,type:switchpro,hint:!SDL_GAMECONTROLLER_USE_GAMECUBE_LABELS:=1,crc:67fa,platform:Windows,");

    // A HIDAPI GUID: the first binding to a button from dpup on never reads
    // its own number, or SDL would move every button input.
    const std::string hid = "050000005e040000e002000000006800";
    CHECK_OR_THROW(HidapiGuid(hid) && !HidapiGuid(guid));
    const Bindings own = {Bind("dpup", B(11)), Bind("dpdown", H(0, 4)), Bind("misc1", B(15))};
    CHECK_OR_THROW(WriteSdlMapping(hid, "Pad", {own}).value().find(",Pad,dpdown:h0.4,dpup:b11,misc1:b15,") != std::string::npos);
    CHECK_OR_THROW(WriteSdlMapping(guid, "Pad", {own}).value().find(",Pad,dpup:b11,dpdown:h0.4,misc1:b15,") != std::string::npos);
    const Bindings legacy = {Bind("a", B(0)), Bind("dpup", B(11)), Bind("dpdown", B(12)), Bind("dpleft", B(13)), Bind("dpright", B(14)), Bind("misc1", B(15))};
    CHECK_OR_THROW(!WriteSdlMapping(hid, "Pad", {legacy}) && WriteSdlMapping(guid, "Pad", {legacy}));
}
// SDL holds the mapping for a virtual pad; what SDL makes of it, read as
// SaveSetup reads it.
SdlSnapshot Snap(SDL_JoystickID pad, const std::string& mapping) {
    if (SDL_AddGamepadMapping(mapping.c_str()) < 0) throw std::runtime_error("SDL rejected " + mapping);
    return ReadSnapshot(pad, nullptr).value();
}
Bindings Hold(SDL_JoystickID pad, const std::string& mapping) { return Snap(pad, mapping).bindings; }
// What a pad shows beyond its bindings: the labels SDL gives its south, east,
// west and north buttons, the type SDL gives the open gamepad, and the brand
// Ember detects from the type it reads by id.
struct Shown {
    std::array<SDL_GamepadButtonLabel, 4> labels;
    SDL_GamepadType type;
    PromptBrand brand;
    bool operator==(const Shown& b) const { return labels == b.labels && type == b.type && brand == b.brand; }
};
Shown ShownOf(SDL_JoystickID pad) {
    SDL_Gamepad* gamepad = SDL_OpenGamepad(pad); CHECK_OR_THROW(gamepad);
    Shown shown;
    for (int i = 0; i < 4; ++i) shown.labels[i] = SDL_GetGamepadButtonLabel(gamepad, static_cast<SDL_GamepadButton>(SDL_GAMEPAD_BUTTON_SOUTH + i));
    shown.type = SDL_GetGamepadType(gamepad);
    SDL_CloseGamepad(gamepad);
    Pad model; model.type = PadType(pad);
    shown.brand = DetectBrand(model);
    return shown;
}
// Merges the recorded steps over what SDL makes of body, as SaveSetup does,
// writing for guid, and checks SDL binds the result exactly as expected and
// shows the pad as it showed body. A HIDAPI-style guid's mapping is decoded on
// the virtual pad, which has no fixup.
std::string Merge(SDL_JoystickID pad, const SetupState& recorded, const std::string& body, const std::string& expected, std::string guid = {}) {
    const auto padGuid = GuidOf(pad);
    if (guid.empty()) guid = padGuid;
    const auto existing = Snap(pad, padGuid + ",Pad," + body);
    const auto shown = ShownOf(pad);
    auto written = SetupMapping(recorded, existing, guid, "Pad", MappingText(padGuid)).value();
    written.replace(0, guid.size(), padGuid);
    const auto decoded = Hold(pad, written);
    const bool sameShown = ShownOf(pad) == shown;
    const auto want = Hold(pad, padGuid + ",Pad," + expected + "," + std::string(PositionalHint));
    if (!(decoded == want) || !sameShown || (HidapiGuid(guid) && LegacyHidapi(decoded))) throw std::runtime_error("merge " + body + " gave " + written);
    if (guid == padGuid) CHECK_OR_THROW(decoded == MergeSetup(recorded, existing.bindings));
    return written;
}
void VirtualPads() {
    const auto pad = Attach("Ember mapping corpus");
    const auto padGuid = GuidOf(pad);
    // Each binding SDL makes is written back as text SDL binds the same way,
    // with the labels, type and brand SDL gave it: SDL's database mappings,
    // composite hats, metadata, case and spaces, lenient inputs, colons, double
    // signs, SDL's stop, label hints, a Sony type without a face, a GameCube
    // hint, and a type past SDL's stop or with a sign.
    const std::string corpus[] = {
        "a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,guide:b10,leftshoulder:b4,leftstick:b8,lefttrigger:a2,leftx:a0,lefty:a1,"
            "rightshoulder:b5,rightstick:b9,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,",
        "crc:c77a,a:b2,b:b1,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,"
            "rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2~,righty:a3~,start:b9,x:b3,y:b0,",
        "leftshoulder:h0.3,rightshoulder:h0.4,lefttrigger:h0.6,a:b0,crc:b003,type:b4,sdk>=:2.0,face:abxy,x:junk,",
        "LEFTSHOULDER:h0.1,A:b0,RightShoulder:b5,+LeftX:a0,LEFTTRIGGER:a2,Platform:Windows,",
        "leftshoulder: h0.1,a :b0, right shoulder:b5, l e f t x : - a 1 ~ ,",
        "leftshoulder:-h0.1,x:+b3,y:b4~,b:h0.2~,a:b5junk,+leftx:+a0junk~,dpup:h0.1junk,dpdown:h0.17,",
        "leftshoulder:h9.1:h0,x:b12:b3,y:h1.2:,a::b4,b:b6::,dpup:h0.1:h1:,",
        "++leftx:a0,+-lefty:a1,-+righty:a2,--rightx:a3,+a:b1,+-b:b2,+lefttrigger:a4,-righttrigger:a5~,++x:b3,",
        "leftshoulder:h0.1," + std::string(21, 'k') + ":v,lefttrigger:h0.1,",
        "a:B3,b:a,x:a-1,y:h0,leftshoulder:h0.0,rightshoulder:h10.1,leftstick:b99999,rightstick:b003,misc1:h1.15,touchpad:-a3~,",
        "\xC5\xBFtart:b9,bac\xE2\x84\xAA:b6,left\xEF\xAC\x86ick:b7,guide:b10,",
        "leftshoulder,dpup,,  ,:,a:b0,a:b1,A:b0,a:b0,",
        "crc:b003,hint:SDL_X:=1,type:xbox360,sdk>=:21,sdk<=:99,x:b2,platform:Windows,",
        "a:b0,b:b1,x:b2,y:b3,hint:SDL_GAMECONTROLLER_USE_BUTTON_LABELS:=1,",
        "a:b0,b:b1,x:b2,y:b3,hint:!SDL_GAMECONTROLLER_USE_GAMECUBE_LABELS:=1,hint:SDL_GAMECONTROLLER_USE_BUTTON_LABELS:=1,",
        "a:b0,x:b2",
        "a:b0,b:b1,x:b2,y:b3,type:ps4,",
        "a:b0,b:b1,x:b2,y:b3,hint:SDL_GAMECONTROLLER_USE_GAMECUBE_LABELS:=1,",
        "a:b0,b:b1,x:b2,y:b3,platform:Windows,platform:Windows,type:ps4,",
        "a:b0,b:b1,x:b2,y:b3,type:+ps4,"};
    std::vector<std::string> written;
    std::vector<Shown> shown;
    for (const auto& body : corpus) {
        const auto held = Snap(pad, padGuid + ",Corpus," + body);
        shown.push_back(ShownOf(pad));
        written.push_back(WriteSdlMapping(padGuid, "Corpus", held, MappingText(padGuid)).value());
        if (!(Hold(pad, written.back()) == held.bindings) || !(ShownOf(pad) == shown.back()) || Count(written.back(), "face:") != 1 || Count(written.back(), "type:") > 1)
            throw std::runtime_error("round trip " + body);
    }
    // The types SDL gives them: PS4 from a type field, even past SDL's stop at
    // a second platform or with a sign, and Ember's PS4 brand with it. A pad
    // with no type field keeps the device's own, which the mapping does not pin.
    for (const int typed : {16, 18, 19})
        CHECK_OR_THROW(shown[typed].type == SDL_GAMEPAD_TYPE_PS4 && shown[typed].brand == PromptBrand(Brand::PlayStation, PlayStationArt::PS4) && Count(written[typed], ",type:ps4,") == 1);
    CHECK_OR_THROW(shown[0].type == SDL_GAMEPAD_TYPE_STANDARD && Count(written[0], "type:") == 0 && Count(written[12], ",type:xbox360,") == 1);
    // The faces SDL gives them: Sony from the type, BAYX from a button label
    // hint, and AXBY from any GameCube hint, even a negated one.
    CHECK_OR_THROW(Snap(pad, padGuid + ",Corpus," + corpus[16]).face == SdlFace::Sony && Snap(pad, padGuid + ",Corpus," + corpus[17]).face == SdlFace::Axby);
    CHECK_OR_THROW(Snap(pad, padGuid + ",Corpus," + corpus[13]).face == SdlFace::Bayx && Snap(pad, padGuid + ",Corpus," + corpus[14]).face == SdlFace::Axby);
    CHECK_OR_THROW(Snap(pad, padGuid + ",Corpus," + corpus[0]).face == SdlFace::Abxy);

    // SDL's database: the 2026-10-13 save over its XInput mapping keeps LB, LT,
    // LS, RS, both sticks and Guide for the skipped steps, in SDL's order.
    const auto owner = Record({{"up", {RawKind::HatUp, 0}}, {"down", {RawKind::HatDown, 0}}, {"left", {RawKind::HatLeft, 0}}, {"right", {RawKind::HatRight, 0}},
        {"lp", {RawKind::Button, 2}}, {"mp", {RawKind::Button, 3}}, {"hp", {RawKind::Button, 5}}, {"lk", {RawKind::Button, 0}}, {"mk", {RawKind::Button, 1}},
        {"hk", {RawKind::Trigger, 5}}, {"start", {RawKind::Button, 7}}, {"select", {RawKind::Button, 6}}});
    Merge(pad, owner, corpus[0], "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,x:b2,y:b3,rightshoulder:b5,a:b0,b:b1,righttrigger:a5,start:b7,back:b6,"
        "guide:b10,leftshoulder:b4,leftstick:b8,lefttrigger:a2,leftx:a0,lefty:a1,rightstick:b9,rightx:a3,righty:a4");
    // Its DirectInput stick with a CRC and inverted axes: LK on Y's button takes target a and drops y.
    const auto stick = Merge(pad, Record({{"up", {RawKind::HatUp, 0}}, {"lk", {RawKind::Button, 0}}}), corpus[1],
        "dpup:h0.1,a:b0,b:b1,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftshoulder:b4,leftstick:b10,lefttrigger:b6,leftx:a0,lefty:a1,"
        "rightshoulder:b5,rightstick:b11,righttrigger:b7,rightx:a2~,righty:a3~,start:b9,x:b3");
    CHECK_OR_THROW(Count(stick, "crc:") == 1 && Count(stick, "platform:") == 1 && Count(stick, "hint:") == 1);

    // Round 8: legacy HIDAPI d-pad buttons with misc1. SDL's fixup turns
    // dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,misc1:b15 into the four h0
    // directions and misc1:b11; a virtual pad has no fixup, so it holds that.
    // Written for a HIDAPI GUID, misc1 stays on button 11 and SDL would not fix it up again.
    const std::string hid = "050000005e040000e002000000006800";
    const auto dpad = Record({{"up", {RawKind::HatUp, 0}}, {"down", {RawKind::HatDown, 0}}, {"left", {RawKind::HatLeft, 0}}, {"right", {RawKind::HatRight, 0}},
        {"lk", {RawKind::Button, 3}}});
    const auto fixedUp = Merge(pad, dpad, "a:b0,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,misc1:b11,",
        "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,a:b3,misc1:b11", hid);
    CHECK_OR_THROW(fixedUp.find(",misc1:b11,") != std::string::npos);
    // Up recorded on button 11 under a HIDAPI GUID: another d-pad binding goes first.
    auto ownUp = dpad; ownUp.recorded[UpStep] = RawInput{RawKind::Button, 11};
    Merge(pad, ownUp, "misc1:b15,", "dpdown:h0.4,dpup:b11,dpleft:h0.8,dpright:h0.2,a:b3,misc1:b15", hid);
    // Round 8: two label hints. SDL binds a:b0 to east and y:b3 to west; LK
    // recorded on button 3 lands on south and drops west.
    const auto labels = Merge(pad, Record({{"lk", {RawKind::Button, 3}}}), corpus[14], "a:b3,b:b0,y:b2");
    CHECK_OR_THROW(Hold(pad, labels)[0] == Bind("a", B(3)) && Count(labels, "BUTTON_LABELS") == 0);
    // Round 8: SDL's returned text stops at a second platform, but SDL still
    // binds leftshoulder:b4, so a skipped 3P keeps it.
    auto skip3p = Record({{"up", {RawKind::HatUp, 0}}, {"lk", {RawKind::Button, 3}}}); skip3p.skipped[Step("3p")] = true;
    const std::string platforms = "a:b0,platform:Windows,platform:Windows,leftshoulder:b4,";
    Hold(pad, padGuid + ",Pad," + platforms); CHECK_OR_THROW(MappingText(padGuid).find("leftshoulder") == std::string::npos);
    Merge(pad, skip3p, platforms, "dpup:h0.1,a:b3,leftshoulder:b4");
    // Kept bindings of every shape come back exactly.
    Merge(pad, Record({{"lk", {RawKind::Button, 3}}}), "Y:+b4,++lefty:a1junk~,LeftTrigger:-a2~,rightx:h1.17,touchpad:+a5~,",
        "a:b3,y:b4,+lefty:a1~,lefttrigger:-a2~,rightx:h1.17,touchpad:+a5~");
    SDL_DetachVirtualJoystick(pad);

    // An unmapped pad binds nothing; the saved setup is exactly the recorded
    // steps, labelled as SDL labels the pad's type.
    const auto unmapped = Attach("Ember unmapped pad");
    const auto snapshot = ReadSnapshot(unmapped, nullptr);
    CHECK_OR_THROW(!SDL_IsGamepad(unmapped) && snapshot && snapshot->bindings.empty());
    const auto type = SDL_GetGamepadTypeForID(unmapped);
    const auto recorded = Record({{"up", {RawKind::HatUp, 0}}, {"lk", {RawKind::Button, 3}}, {"hk", {RawKind::Trigger, 5, false, true}}});
    CHECK_OR_THROW(Hold(unmapped, SetupMapping(recorded, snapshot, GuidOf(unmapped), "Unmapped", {}).value()) ==
        Bindings({Bind("dpup", H(0, 1)), Bind("a", B(3)), Bind("righttrigger", A(5, Hi, Lo))}));
    const auto shownUnmapped = ShownOf(unmapped);
    CHECK_OR_THROW(shownUnmapped.type == type && snapshot->type.empty());
    for (int i = 0; i < 4; ++i) CHECK_OR_THROW(shownUnmapped.labels[i] == SDL_GetGamepadButtonLabelForType(type, static_cast<SDL_GamepadButton>(SDL_GAMEPAD_BUTTON_SOUTH + i)));
    // A read SDL fails is no snapshot, not a pad that binds nothing, and a save from it writes no mapping.
    CHECK_OR_THROW(!ReadBindings(nullptr) && !SetupMapping(recorded, std::nullopt, GuidOf(unmapped), "Unmapped", {}));
    SDL_DetachVirtualJoystick(unmapped);

    // A GameCube pad: SDL swaps b and x of a mapping without a GameCube hint
    // and labels it AXBY; the positional mark keeps them where the merge put
    // them, and the face keeps its labels.
    const auto cube = Attach("Ember GameCube pad", 0x1234, 0x5678);
    const auto cubeGuid = GuidOf(cube);
    CHECK_OR_THROW(Hold(cube, cubeGuid + ",Cube,x:b2,b:b1,") == Bindings({Bind("b", B(2)), Bind("x", B(1))}));
    Merge(cube, Record({{"lp", {RawKind::Button, 2}}, {"mk", {RawKind::Button, 5}}}), "a:b0,b:b1,", "x:b2,b:b5,a:b0");
    SDL_DetachVirtualJoystick(cube);

    // A pad SDL finds to be a PS4 by its VID and PID: a save without a type
    // field keeps that without pinning it, and a standard type a mapping gives
    // over it is written, so the type never moves.
    const auto sony = Attach("Ember Sony pad", 0x054c, 0x09cc);
    const auto lk = Record({{"lk", {RawKind::Button, 3}}});
    CHECK_OR_THROW(Count(Merge(sony, lk, "a:b0,b:b1,", "a:b3,b:b1"), "type:") == 0 && PadType(sony) == Type::PS4);
    CHECK_OR_THROW(Count(Merge(sony, lk, "a:b0,b:b1,type:standard,", "a:b3,b:b1"), ",type:standard,") == 1);
    SDL_DetachVirtualJoystick(sony);
}
int main() {
    Overlap();
    PureMerge();
    Writing();
    nlohmann::json prefs = {{"controllerMappings", {{"03000000790000001100000000000000", "guid,Pad,a:b0,"}}}};
    CHECK_OR_THROW(sf4e::netplay::ReadControllerMappings(nlohmann::json::parse(prefs.dump())).at("03000000790000001100000000000000") == "guid,Pad,a:b0,");
    CHECK_OR_THROW(sf4e::netplay::ReadControllerMappings({{"deviceType", 0}, {"deviceIdx", 2}}).empty());
    CHECK_OR_THROW(sf4e::netplay::ReadControllerMappings({{"controllerMappings", {{"03000000790000001100000000000000", 5}}}}).empty());
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_GAMECUBE_DEVICES, "0x1234/0x5678");
    CHECK_OR_THROW(SDL_Init(SDL_INIT_GAMEPAD));
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) CHECK_OR_THROW(SdlButtonNames[i] == SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(i)));
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i) CHECK_OR_THROW(SdlAxisNames[i] == SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(i)));
    VirtualPads();
    SDL_Quit(); std::cout << "Merges, mappings and SDL's bindings passed\n";
}
