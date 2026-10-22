#include "../ui/RematchPanel.hxx"
#include "imgui_test_support.hxx"
#include "test_support.hxx"
#include <iostream>

int main() {
    using namespace sf4e;
    using namespace sf4e::ui;
    using rematchpanel::Choice;
    HeadlessImGui context;
    RematchPanel panel;
    rematchpanel::View view;view.offer=41;view.visible=view.canRematch=true;
    std::vector<MenuEntry> rows;std::string status;
    SetMenuEntriesProbe([&](const std::vector<MenuEntry>& entries){rows=entries;});
    SetMenuStatusProbe([&](const char* text,Tone){status=text;});
    const auto frame=[&](unsigned held=0) {
        ImGui::GetIO().DeltaTime=1.f/60;
        SetMenuInput({held,0});ImGui::NewFrame();
        const auto choice=panel.Draw(view);ImGui::Render();return choice;
    };
    // A held victory-screen Continue cannot accept the newly offered rematch.
    CHECK(frame(MenuInput::Select)==Choice::None);
    CHECK(frame(MenuInput::Select)==Choice::None);
    CHECK(rows.size()==2&&rows[0].id=="quick-rematch"&&rows[1].id=="quick-lobby");
    CHECK(rows[1].label==loc::T("quick_rematch.return_lobby"));
    CHECK(frame()==Choice::None&&frame(MenuInput::Select)==Choice::Rematch);
    CHECK(frame()==Choice::None&&frame(MenuInput::Select)==Choice::None); // no duplicate while queued
    view.accepted=true;
    CHECK(frame()==Choice::None&&frame(MenuInput::Back)==Choice::Lobby); // cancellation remains possible
    CHECK(!rows[0].enabled&&rows[1].enabled);
    CHECK(frame()==Choice::None&&frame(MenuInput::Back)==Choice::None); // one return request

    // The new offer cannot inherit a held Back or the previous offer's request.
    ++view.offer;view.accepted=false;
    CHECK(frame(MenuInput::Back)==Choice::None);
    CHECK(frame()==Choice::None&&frame(MenuInput::Back)==Choice::Lobby);

    // A disabled rematch still leaves the second explicit action reachable.
    ++view.offer;view.canRematch=false;
    CHECK(frame()==Choice::None&&frame(MenuInput::Select)==Choice::None);
    CHECK(frame()==Choice::None&&frame(MenuInput::Down)==Choice::None);
    CHECK(frame()==Choice::None&&frame(MenuInput::Select)==Choice::Lobby);

    // A full command queue is retryable after releasing the initiating press.
    ++view.offer;view.canRematch=true;
    CHECK(frame()==Choice::None&&frame(MenuInput::Select)==Choice::Rematch);
    panel.QueueFailed();
    CHECK(frame(MenuInput::Select)==Choice::None&&status==loc::T("error.queue_failed"));
    CHECK(frame()==Choice::None&&frame(MenuInput::Select)==Choice::Rematch);

    // Online waiting retains Lobby, and no fighter-selection shortcut exists.
    ++view.offer;view.accepted=true;view.status="Waiting for the other player";
    CHECK(frame()==Choice::None);
    CHECK(rows.size()==2&&!rows[0].enabled&&rows[1].enabled&&rows[1].label==loc::T("quick_rematch.return_lobby"));
    CHECK(frame(MenuInput::Fighter)==Choice::None&&frame()==Choice::None);
    CHECK(frame(MenuInput::Back)==Choice::Lobby);
    view.offer=0;CHECK(!rematchpanel::Shown(view)&&frame()==Choice::None);
    view.offer=52;view.visible=false;CHECK(!rematchpanel::Shown(view)&&frame()==Choice::None);
    SetMenuEntriesProbe({});SetMenuStatusProbe({});
    std::cout<<"Rematch offer, held input, two actions, cancellation and queue retry passed\n";
}
