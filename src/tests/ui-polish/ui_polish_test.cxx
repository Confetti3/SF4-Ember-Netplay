#include "../../ui/MenuFeedback.hxx"
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace sf4e::ui;
static unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { std::cerr << "FAIL line " << __LINE__ << ": " #value "\n"; std::exit(1); } } while (false)

static std::vector<MenuEntry> Entries() {
    MenuEntry edit; edit.id="name"; edit.label="Room name"; edit.text=true; edit.value="Ember"; edit.textLimit=64;
    MenuEntry confirm; confirm.id="leave"; confirm.label="Leave room"; confirm.confirm=true;
    MenuEntry adjust; adjust.id="delay"; adjust.label="Delay"; adjust.adjustable=true;
    return {edit, confirm, adjust};
}
static void TestDraftAndConfirmation() {
    auto entries=Entries(); MenuNavigation nav;
    CHECK(nav.Focus().empty());
    nav.Reconcile(entries); nav.Choose(entries); nav.Draft("My room");
    entries[0].enabled=false; entries[0].pending=true;
    nav.Reconcile(entries);
    CHECK(nav.Editing()); CHECK(nav.Draft()=="My room");
    CHECK(nav.AcceptText(entries).kind==MenuAction::None); CHECK(nav.Editing());
    nav.Focus("leave",entries); CHECK(nav.Focus()=="name");
    entries[0].enabled=true;
    const auto accepted=nav.AcceptText(entries);
    CHECK(accepted.kind==MenuAction::TextAccepted); CHECK(accepted.text=="My room");
    CHECK(!nav.Editing());
    CHECK(nav.Update({MenuInput::Select,1},entries).kind==MenuAction::None);
    CHECK(!nav.Editing());
    nav.Update({0,1.1},entries);
    nav.Focus("leave",entries); nav.Choose(entries);
    CHECK(nav.Confirming()); CHECK(nav.DialogId()=="leave");
    entries[1].enabled=false; entries[1].pending=true;
    CHECK(nav.Confirm(true,entries).kind==MenuAction::None); CHECK(nav.Confirming());
    nav.Focus("name",entries); CHECK(nav.Focus()=="leave");
    entries[1].enabled=true;
    CHECK(nav.Confirm(true,entries).id=="leave"); CHECK(!nav.Confirming());
    nav.Focus("name",entries); nav.Choose(entries); nav.Draft("Unsaved");
    entries[0].enabled=false; entries[0].pending=false; nav.Reconcile(entries);
    CHECK(!nav.Editing()); CHECK(nav.Draft().empty());
    entries[0].enabled=true; nav.Choose(entries); entries.erase(entries.begin()); nav.Reconcile(entries);
    CHECK(!nav.Editing());
    nav.Focus("leave",entries); nav.Choose(entries); entries[0].confirm=false; nav.Reconcile(entries);
    CHECK(!nav.Confirming());
}
static void TestDeferredText() {
    auto entries=Entries(); MenuNavigation nav; nav.Reconcile(entries); nav.Choose(entries);
    nav.NeutralGate();
    CHECK(nav.Update({MenuInput::Select,0,true},entries,1,true,true).kind==MenuAction::None);
    CHECK(nav.Editing());
    nav.Update({0,.1},entries,1,true,true);
    CHECK(nav.Update({MenuInput::Select,.2,true},entries,1,true,true).kind==MenuAction::SubmitText);
    CHECK(nav.Editing());
    nav.Draft("Last character processed");
    CHECK(nav.AcceptText(entries).text=="Last character processed");
    nav.Choose(entries); nav.Update({0,.3},entries,1,true,true);
    CHECK(nav.Update({MenuInput::Back,.4,true},entries,1,true,true).kind==MenuAction::None);
    CHECK(!nav.Editing());
}
static void TestNavigation() {
    auto entries=Entries(); MenuNavigation nav;
    nav.Reconcile(entries); nav.Push("settings"); CHECK(nav.Focus().empty());
    nav.Push("settings"); CHECK(nav.Parent()=="home");
    nav.Update({MenuInput::Select,0},entries); CHECK(!nav.Editing());
    nav.Update({0,.1},entries); nav.Update({MenuInput::Down,.2},entries);
    CHECK(nav.Focus()=="leave");
    nav.NeutralGate(); nav.Update({0,10},entries); nav.Update({MenuInput::Down,10.01},entries);
    CHECK(nav.Focus()=="delay");
    nav.Focus("name",entries);
    nav.Update({MenuInput::Down,10.10},entries); CHECK(nav.Focus()=="name");
    nav.Update({0,11},entries); nav.Focus("delay",entries);
    CHECK(nav.Update({MenuInput::Up|MenuInput::Right,11.1},entries).kind==MenuAction::None);
    CHECK(nav.Focus()=="leave");
    nav.Update({0,12},entries); nav.Update({MenuInput::Down,12.1},entries,0);
    CHECK(nav.Focus()=="delay");
    nav.Scroll()=55; CHECK(nav.Return().kind==MenuAction::Returned); CHECK(nav.Screen()=="home");
    CHECK(nav.Update({MenuInput::Back,13},entries).kind==MenuAction::None);
    nav.Push("settings"); CHECK(nav.Scroll()==55);
}
static void TestFeedback() {
    auto entries=Entries(); MenuVisualFeedback feedback;
    feedback.Update("room",entries,0); CHECK(feedback.Enabled(entries[0]));
    for (int frame=1; frame<=120; ++frame) {
        entries[0].enabled=frame%2==0; entries[0].pending=!entries[0].enabled;
        feedback.Update("room",entries,frame/60.0);
        CHECK(feedback.Enabled(entries[0]));
        MenuNavigation nav; nav.Reconcile(entries);
        if (!entries[0].enabled) { nav.Choose(entries); CHECK(!nav.Editing()); }
    }
    entries[0].enabled=false; entries[0].pending=true;
    feedback.Update("room",entries,3); feedback.Update("room",entries,3.3);
    CHECK(!feedback.Enabled(entries[0]));
    for(int frame=1;frame<=60;++frame) {
        entries[0].enabled=frame%2!=0; entries[0].pending=!entries[0].enabled; feedback.Update("room",entries,3.3+frame/60.0);
        CHECK(!feedback.Enabled(entries[0]));
        CHECK(feedback.Pending(entries[0]));
    }
    entries[0].enabled=true; feedback.Update("room",entries,5); feedback.Update("room",entries,5.3);
    CHECK(feedback.Enabled(entries[0]));
    entries[0].enabled=false; entries[0].pending=false; feedback.Update("room",entries,5.31);
    CHECK(!feedback.Enabled(entries[0]));
    entries[0].enabled=true; feedback.Update("room",entries,6);
    entries[0].enabled=false; entries[0].pending=true; entries[0].label="Match in progress";
    feedback.Update("room",entries,6.01); CHECK(!feedback.Enabled(entries[0]));
    entries[0].enabled=true; feedback.Update("other-room",entries,6.02); CHECK(feedback.Enabled(entries[0]));
    feedback.Update("other-room",{},6.03); entries[0].enabled=false;
    feedback.Update("other-room",entries,6.04); CHECK(!feedback.Enabled(entries[0]));
}
static void TestClock() {
    CHECK(RebaseUiTimestamp(100.45,100.1,.1) > .4);
    CHECK(RebaseUiTimestamp(97,100,.1) < .1-2); // Already-overdue save stays overdue.
    UiClock clock; CHECK(clock.Update(100)==100);
    const double deadline=100.45;
    CHECK(clock.Update(100.1)<deadline);
    CHECK(clock.Update(.1)>100.1); // Context recreation must not restart save/notice deadlines.
    CHECK(clock.Update(.5)>deadline);
    const auto before=clock.Update(1);
    CHECK(clock.Update(std::numeric_limits<double>::quiet_NaN())==before);
    CHECK(clock.Update(-1)==before);
}

// Regression for the reported "room options glitch ... frantic clicking or
// button-mashing". The room board used to paint straight from entry.enabled and
// so flickered once per checkpoint. The smoothed verdict the board now reads
// must stay put while a healthy room churns, and must still follow a real change.
static void TestBoardFlickerBound() {
    auto entries=Entries(); MenuVisualFeedback feedback;
    feedback.Update("room",entries,0);
    bool previous=feedback.Enabled(entries[1]);
    unsigned rendered=0, raw=0; bool rawPrevious=entries[1].enabled;
    // Ten seconds of 60fps checkpoint churn on the Leave row.
    for (int frame=1; frame<=600; ++frame) {
        const double now=frame/60.0;
        entries[1].enabled=frame%2==0; entries[1].pending=true;
        if (entries[1].enabled!=rawPrevious) { ++raw; rawPrevious=entries[1].enabled; }
        feedback.Update("room",entries,now);
        const bool shown=feedback.Enabled(entries[1]);
        if (shown!=previous) { ++rendered; previous=shown; }
    }
    CHECK(raw>500);      // The underlying gate really is churning.
    CHECK(rendered==0);  // The player sees a steady button, not a strobe.

    // A sustained loss of eligibility must still reach the player.
    entries[1].enabled=false; entries[1].pending=true;
    for (int frame=1; frame<=60; ++frame) feedback.Update("room",entries,10+frame/60.0);
    CHECK(!feedback.Enabled(entries[1]));
    CHECK(feedback.Pending(entries[1]));

    // ...and so must its return, once it holds.
    entries[1].enabled=true; entries[1].pending=false;
    feedback.Update("room",entries,12); feedback.Update("room",entries,12.5);
    CHECK(feedback.Enabled(entries[1]));
}
int main() {
    TestDraftAndConfirmation(); TestDeferredText(); TestNavigation(); TestFeedback(); TestBoardFlickerBound(); TestClock();
    std::cout << checks << " UI polish checks passed\n";
}
