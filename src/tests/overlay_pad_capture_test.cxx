// The game's pads have two owners: Ember's menu (MenuPads) and the training
// controls. The menu's keyboard and mouse capture is the window's alone and
// never holds the pads: once the drawing thread has stopped with a focused
// menu shown, a match being prepared gives the pads back to the game at once,
// and behind another window the menu still holds them at the main menu.
#include "../sf4e/sf4e__Overlay.hxx"
#include "../sf4e/sf4e__OverlayPrefs.hxx"
#include "../netplay/ControllerRestore.hxx"
#include <atomic>
#include <chrono>
#include <thread>

#include "test_support.hxx"

int main() {
    {
        namespace input = sf4e::input;
        namespace prefs = sf4e::OverlayPrefs;
        input::Device keyboard{input::PadKeyboard, 0, "Keyboard", true};
        input::Device pad{input::PadXInput, 2, "Controller 3", true};
        pad.identity = {"guid", "serial", "path", "Recognised pad"};
        // The old slot save adopts the recognised occupant and round-trips its identity.
        prefs::Data old;
        prefs::FromJson({{"device", {{"type", input::PadXInput}, {"idx", 2}}}}, old);
        input::Device saved; saved.type = old.deviceType; saved.index = old.deviceIdx;
        auto restored = input::RestoreController({keyboard, pad}, keyboard, saved, {});
        CHECK(!restored.provisional && restored.device.Same(pad));
        old.deviceIdentity = restored.device.identity;
        prefs::Data migrated;
        prefs::FromJson(prefs::ToJson(old), migrated);
        CHECK(migrated.deviceIdentity && *migrated.deviceIdentity == pad.identity);
        saved.identity = *migrated.deviceIdentity;
        auto other = pad; other.identity.serial = "other";
        restored = input::RestoreController({keyboard, other}, other, saved, {});
        CHECK(restored.provisional && restored.device.Same(other));
        pad.index = 0;
        restored = input::RestoreController({keyboard, other, pad}, other, saved, {});
        CHECK(!restored.provisional && restored.device.Same(saved) && restored.device.index == 0);
        CHECK(*migrated.deviceIdentity == saved.identity);
        // Capture follows the candidate's identity while it releases in a different slot.
        input::Assignment capture;
        capture.Begin(); capture.Tick({keyboard, pad});
        pad.buttons = 1; capture.Tick({keyboard, pad});
        CHECK(capture.State() == input::Capture::ReleaseSelected);
        pad.index = 1; pad.buttons = 0;
        CHECK(capture.Tick({keyboard, pad}) && capture.Selected().index == 1);
        // Unmapped DirectInput choices keep the original type/index persistence.
        prefs::Data direct;
        direct.deviceType = input::PadDirectInput; direct.deviceIdx = 5;
        prefs::Clamp(direct);
        prefs::Data roundTrip;
        prefs::FromJson(prefs::ToJson(direct), roundTrip); prefs::Clamp(roundTrip);
        CHECK(roundTrip.deviceType == input::PadDirectInput && roundTrip.deviceIdx == 5 && !roundTrip.deviceIdentity);
    }
    namespace Overlay = sf4e::Overlay;
    using sf4e::input::MenuContext;
    auto& pads = Overlay::MenuPads();
    // In front at the main menu: Start asks, and the request holds the pads
    // until the frame that takes it has shown the menu.
    pads.Place(MenuContext::MainMenu, false);
    CHECK(Overlay::HasInputFocus() && !Overlay::CapturesMenuInput());
    Overlay::RequestMenuStart();
    CHECK(Overlay::CapturesMenuInput());
    pads.Publish(true, pads.Take());
    CHECK(Overlay::CapturesMenuInput());
    // Drawing stops there. A match is prepared, played and over: the pads are
    // the game's throughout, whatever the last frame said.
    pads.Place(MenuContext::MainMenu, true); CHECK(!Overlay::CapturesMenuInput());
    pads.Place(MenuContext::Unavailable, true); CHECK(!Overlay::CapturesMenuInput());
    pads.Place(MenuContext::Unavailable, false); CHECK(!Overlay::CapturesMenuInput());
    // Back at the main menu behind another window: the menu, still shown,
    // holds the pads again; hidden, Start there asks for it and holds them.
    Overlay::OverlayWindowFunc(nullptr, WM_ACTIVATEAPP, FALSE, 0);
    CHECK(!Overlay::HasInputFocus());
    pads.Place(MenuContext::MainMenu, false); CHECK(Overlay::CapturesMenuInput());
    pads.Publish(false, pads.Take()); CHECK(!Overlay::CapturesMenuInput());
    Overlay::RequestMenuStart(); CHECK(Overlay::CapturesMenuInput());
    // Start is also the way back to a watched game, through the real overlay
    // request entry point, even when another window has focus. The hide holds
    // the pads until the drawing thread answers it.
    pads.Publish(true, pads.Take());
    pads.Place(MenuContext::WatchedMatch, true);
    Overlay::RequestMenuStart(); CHECK(Overlay::CapturesMenuInput());
    const auto hide = pads.Take();
    CHECK(hide.kind == sf4e::ui::MenuRequest::Toggle);
    CHECK(Overlay::CapturesMenuInput());
    pads.Publish(false, hide); CHECK(!Overlay::CapturesMenuInput());
    Overlay::RequestMenuStart(); CHECK(Overlay::CapturesMenuInput());
    const auto reopen = pads.Take();
    CHECK(reopen.kind == sf4e::ui::MenuRequest::Toggle);
    pads.Publish(true, reopen);
    pads.Place(MenuContext::MainMenu, false);
    // A display reset while a frame is still drawing. The frame took Start's
    // open, another Start comes after it, and the frame publishes the menu
    // shown only once the teardown has begun. The teardown waits for the
    // frame and then drops what was shown and asked, so the pads are the
    // game's when it completes; the next pad update and frame decide again.
    std::atomic<bool> drawing{false};
    std::thread frame([&] {
        const sf4e::ui::OverlayLifecycle::Frame guard(Overlay::Lifecycle());
        const auto taken = pads.Take();
        Overlay::RequestMenuStart();
        drawing = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        pads.Publish(true, taken);
    });
    while (!drawing) std::this_thread::yield();
    Overlay::FreeOverlay();
    frame.join();
    CHECK(!Overlay::CapturesMenuInput());
    return 0;
}
