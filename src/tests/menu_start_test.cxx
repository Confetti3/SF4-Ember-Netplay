#include "../ui/OverlayPresentation.hxx"
#include "test_support.hxx"
#include <array>
#include <cstring>
#include <iostream>

using namespace sf4e;
using input::MenuContext;
using netplay::MatchState;
using ui::MenuPads;

// The same handoff as Pad::UpdateInputs and DrawOverlay: sample Start before
// native suppression, answer its request on the drawing thread, then publish
// visibility. Neither thread changes the watched session or its simulation.
struct Watching {
    input::MenuStartInput start;
    input::MenuInputCapture capture;
    MenuPads pads;
    ui::OverlayPresentation menu;
    MenuContext context = MenuContext::WatchedMatch;
    MatchState match = MatchState::Playing;
    int type, index = 0;
    bool focused;
    explicit Watching(int kind, bool focus) : type(kind), focused(focus) {
        menu.Update(context, match, false, focused); menu.Close();
        Poll(false); Poll(false); Draw();
    }
    bool Poll(bool held, bool connected = true, bool enabled = true, bool drawBeforeCapture = false) {
        pads.Place(context, netplay::Fighting(match));
        const unsigned mapped = held ? 0x1000 : 0;
        // A DirectInput stick may bind Start to a button other than the
        // normalized Xbox Start bit. Its mapped Start is what matters.
        const unsigned physical = !held ? 0 : type == input::PadXInput ? input::xinput::Start : 0x80000;
        const bool pressed = start.Update(context, type, index, connected, mapped, physical, enabled);
        if (pressed) pads.RequestStart();
        if (drawBeforeCapture) Draw();
        alignas(4) std::array<unsigned char, 0x100> native{};
        const unsigned nativeStart = held ? input::NativeStart : 0;
        std::memcpy(native.data() + 0x18, &nativeStart, sizeof(nativeStart));
        const bool blocked = capture.Update(pressed || pads.Holds(), input::NativeMenuHeld(native.data()));
        if (blocked) input::ClearNativeMenuInputs(native.data());
        CHECK(!blocked || !input::NativeMenuHeld(native.data()));
        return blocked;
    }
    void Draw() {
        menu.Update(context, match, false, focused);
        const auto request = pads.Take();
        menu.Apply(request.kind, context);
        pads.Publish(menu.Visible(), request);
    }
    void Press() { Poll(false); Poll(true); Draw(); }
};

static void ToggleAndDrain(int type, bool focused) {
    Watching w(type, focused);
    CHECK(!w.menu.Visible() && !w.pads.Holds());
    CHECK(w.Poll(true) && w.pads.Holds()); // captured before the first draw
    w.Draw(); CHECK(w.menu.Visible());
    for (int tick = 0; tick < 10; ++tick) { CHECK(w.Poll(true)); w.Draw(); CHECK(w.menu.Visible()); }
    CHECK(w.Poll(false));
    CHECK(w.Poll(true)); w.Draw(); CHECK(!w.menu.Visible()); // regression: Start used to do nothing here
    for (int tick = 0; tick < 10; ++tick) { CHECK(w.Poll(true)); w.Draw(); CHECK(!w.menu.Visible()); }
    CHECK(!w.Poll(false)); // release, not hiding, returns native input
    CHECK(w.Poll(true)); w.Draw(); CHECK(w.menu.Visible());
    // F10 still follows focus. A fresh Start works after F10 hides the menu.
    w.Poll(false); w.menu.Toggle();
    CHECK(w.menu.Visible() == !focused);
    w.pads.Publish(w.menu.Visible(), w.pads.Take());
    w.Press(); CHECK(w.menu.Visible() == focused);
    CHECK(w.match == MatchState::Playing);
}

static void OwnerAndContext() {
    Watching w(input::PadXInput, true);
    w.Press(); CHECK(w.menu.Visible());
    w.Poll(false); ++w.index;
    w.Poll(true); w.Draw(); CHECK(w.menu.Visible()); // another controller held Start
    w.Press(); CHECK(!w.menu.Visible());
    w.Poll(false, false); w.Poll(true); w.Draw(); CHECK(!w.menu.Visible()); // reconnect held
    w.Press(); CHECK(w.menu.Visible());
    w.Poll(false, true, false); w.Poll(true); w.Draw(); CHECK(w.menu.Visible()); // assignment held
    w.Press(); CHECK(!w.menu.Visible());
    // A queued hide cannot hide the room when a watched game ends.
    w.Press(); CHECK(w.menu.Visible());
    w.Poll(false); w.Poll(true);
    w.context = MenuContext::MainMenu; w.match = MatchState::PostMatch;
    w.Draw(); CHECK(w.menu.Visible());
    w.Poll(true); w.Draw(); CHECK(w.menu.Visible());
    // The next watched game needs neutral before its first Start.
    w.context = MenuContext::WatchedMatch; w.match = MatchState::Playing;
    w.Poll(true); w.Draw(); CHECK(w.menu.Visible());
    w.Press(); CHECK(!w.menu.Visible());
}

static void HandoffAndReset() {
    Watching w(input::PadDirectInput, false);
    w.Press(); CHECK(w.menu.Visible());
    w.Poll(false);
    const auto beforePress = w.pads.Take();
    CHECK(w.Poll(true)); // arrives after this frame took requests
    w.pads.Publish(true, beforePress); CHECK(w.pads.Holds());
    w.Draw(); CHECK(!w.menu.Visible());
    CHECK(w.Poll(true)); w.Draw(); CHECK(!w.menu.Visible());
    w.Press(); CHECK(w.menu.Visible());
    // An explicit open wins over a queued hide (the native Network item).
    w.Poll(false); w.Poll(true); w.pads.Request(MenuPads::Kind::Play);
    w.Draw(); CHECK(w.menu.Visible());
    // A display reset drops a queued request. Held Start must not recreate it.
    w.Poll(false); w.Poll(true); w.pads.Reset();
    CHECK(!w.pads.Holds());
    w.Poll(true); w.Draw(); CHECK(w.menu.Visible());
    w.Press(); CHECK(!w.menu.Visible());
    // A menu just shown by F10 can be hidden before the game thread has ever
    // seen it hold the pads. Even an immediate draw answering that hide must
    // not release its Start into the native menu.
    w.Poll(false); w.menu.Open(); w.pads.Publish(true, w.pads.Take());
    w.capture = input::MenuInputCapture{};
    CHECK(w.Poll(true, true, true, true)); CHECK(!w.menu.Visible());
    CHECK(w.Poll(true)); CHECK(!w.Poll(false));
    // Repeated complete presses during a stalled draw coalesce to one hide.
    w.Press(); CHECK(w.menu.Visible());
    w.Poll(false); w.Poll(true); w.Poll(false); w.Poll(true);
    w.Draw(); CHECK(!w.menu.Visible());
}

static void OtherContexts() {
    for (const auto context : {MenuContext::MainMenu, MenuContext::PracticeMatch}) {
        Watching w(input::PadXInput, true); w.context = context;
        w.match = context == MenuContext::MainMenu ? MatchState::None : MatchState::Playing;
        w.Poll(false); w.Poll(false); w.Press(); CHECK(w.menu.Visible());
        w.Press(); CHECK(w.menu.Visible()); // still open-only
    }
    for (const auto context : {MenuContext::Unavailable, MenuContext::OfflineTraining, MenuContext::ReplayExport}) {
        Watching w(input::PadXInput, true); w.context = context; w.match = MatchState::None;
        w.Poll(false); w.Poll(false); CHECK(!w.Poll(true));
        w.Draw(); CHECK(!w.menu.Visible()); // native pause stays the game's
    }
    input::MenuStartInput xinput, direct;
    const auto context = MenuContext::WatchedMatch;
    for (int tick = 0; tick < 2; ++tick) {
        xinput.Update(context, input::PadXInput, 0, true, 0, 0, true);
        direct.Update(context, input::PadDirectInput, 0, true, 0, 0, true);
    }
    CHECK(xinput.Update(context, input::PadXInput, 0, true, 0x10, input::xinput::Start, true));
    CHECK(!direct.Update(context, input::PadDirectInput, 0, true, 0x10, input::xinput::Start, true));
    CHECK(direct.Update(context, input::PadDirectInput, 0, true, 0x1000, 0x80000, true));
}

int main() {
    for (const auto type : {input::PadXInput, input::PadDirectInput})
        for (const bool focused : {false, true}) ToggleAndDrain(type, focused);
    OwnerAndContext(); HandoffAndReset(); OtherContexts();
    std::cout << "Spectator Start toggle, input drain, owner/context changes and request handoff passed\n";
}
