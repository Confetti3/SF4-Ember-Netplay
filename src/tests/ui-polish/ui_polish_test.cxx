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
    nav.Rearm();
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
    nav.Rearm(); nav.Update({0,10},entries); nav.Update({MenuInput::Down,10.01},entries);
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
// One layout for a value row's drawing and its pointer (LayOutMenuRow,
// MenuValueStep). A value under its label (a long label, a narrow row) has its
// own line: a click on the label's line opens the row, even straight over the
// value's arrow; only the value's own line steps it.
static void TestRowLayout() {
    MenuEntry word; word.id="language"; word.label="Language"; word.adjustable=true; word.value="English";
    const auto under=LayOutMenuRow({0,0,300,48},14,286,90,true,26,8);
    CHECK(under.under && under.row.y1==74 && under.labelLines==1 && under.label.y1==48 && under.value.y0==48 && under.label.x1==286 && under.value.x0==196 && under.value.x1==286);
    // Over the right arrow: on the label's line Select, on the value's a step up.
    CHECK(MenuValueStep(word,under,280,20,0,36,24)==0);
    CHECK(MenuValueStep(word,under,280,60,0,36,24)==1);
    CHECK(MenuValueStep(word,under,200,60,0,36,24)==-1);
    // The word itself opens its list; left of the value is the label's line.
    CHECK(MenuValueStep(word,under,240,60,0,36,24)==0 && MenuValueStep(word,under,100,60,0,36,24)==0);
    // The row's inset past the right arrow still steps it.
    CHECK(MenuValueStep(word,under,295,60,0,36,24)==1);
    // Beside its label the value's line is the row's.
    const auto beside=LayOutMenuRow({0,0,300,48},14,286,90,false,26,8);
    CHECK(!beside.under && beside.row.y1==48 && beside.value.y0==0 && beside.label.x1==188 && MenuValueStep(word,beside,280,10,0,36,24)==1);
    // A number steps on either half, a switch only on itself, a disabled row never.
    MenuEntry number=word; number.value="3";
    CHECK(MenuValueStep(number,under,250,60,0,36,24)==1 && MenuValueStep(number,under,230,60,0,36,24)==-1);
    CHECK(MenuValueStep(word,under,280,60,-1,36,24)==-1 && MenuValueStep(word,under,240,60,-1,36,24)==0);
    MenuEntry off=word; off.enabled=false; CHECK(MenuValueStep(off,under,280,60,0,36,24)==0);
    // A row with no value steps nothing.
    CHECK(MenuValueStep(word,LayOutMenuRow({0,0,300,48},14,286,0,false,26,8),280,10,0,36,24)==0);
}
// A label wider than its whole row (a long translation at 150%), its value
// already under it, wraps to a second line and the row grows by that line: the
// label's two lines over the value's own. A click on the value's line steps
// it; a click on either of the label's lines, even straight over an arrow, is
// Select.
static void TestWrappedLabel() {
    MenuEntry word; word.id="speech-remind"; word.label="Remind me of the describe button"; word.adjustable=true; word.value="Auto";
    const auto wrapped=LayOutMenuRow({0,0,300,48},14,286,90,true,26,8,2);
    CHECK(wrapped.labelLines==2 && wrapped.row.y0==0 && wrapped.row.y1==100);
    CHECK(wrapped.label.y0==0 && wrapped.label.y1==74 && wrapped.label.x0==14 && wrapped.label.x1==286);
    CHECK(wrapped.value.y0==74 && wrapped.value.y1==100 && wrapped.value.x0==196 && wrapped.value.x1==286);
    CHECK(MenuValueStep(word,wrapped,280,90,0,36,24)==1 && MenuValueStep(word,wrapped,200,90,0,36,24)==-1);
    CHECK(MenuValueStep(word,wrapped,280,20,0,36,24)==0 && MenuValueStep(word,wrapped,280,60,0,36,24)==0 && MenuValueStep(word,wrapped,200,60,0,36,24)==0);
    // A switch under a wrapped label flips on its own line only.
    CHECK(MenuValueStep(word,wrapped,280,90,-1,36,24)==-1 && MenuValueStep(word,wrapped,280,60,-1,36,24)==0);
}
int main() {
    TestDraftAndConfirmation(); TestDeferredText(); TestNavigation(); TestFeedback(); TestBoardFlickerBound(); TestClock(); TestRowLayout(); TestWrappedLabel();
    std::cout << checks << " UI polish checks passed\n";
}
