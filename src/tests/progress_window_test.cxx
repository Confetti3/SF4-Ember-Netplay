// Updater.exe's progress window: what it shows of the installer's reports,
// and its worker's start and close.
#include "../launcher/update/ProgressWindow.hxx"
#include <string>
#include <vector>
#include "test_support.hxx"

using sf4e::launcher::InstallerStepCount;
using sf4e::launcher::InstallerSteps;
using sf4e::launcher::ProgressWindow;
using sf4e::launcher::UpdateStage;
using sf4e::launcher::UpdaterNotice;
using sf4e::launcher::InstallActivity;

static HANDLE progressThread=nullptr;
static HANDLE WINAPI TrackProgressThread(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack, LPTHREAD_START_ROUTINE run, LPVOID parameter, DWORD flags, LPDWORD id) {
    const HANDLE thread=CreateThread(attributes,stack,run,parameter,flags,id);
    if(thread) DuplicateHandle(GetCurrentProcess(),thread,GetCurrentProcess(),&progressThread,SYNCHRONIZE,FALSE,0);
    return thread;
}
static HANDLE WINAPI FailProgressThread(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD) { return nullptr; }
static std::wstring Text(HWND control) {
    wchar_t text[256]={};
    GetWindowTextW(control,text,256);
    return text;
}
static std::wstring Digits(const std::wstring& text) {
    std::wstring digits;
    for(const wchar_t c:text) if(c>=L'0' && c<=L'9') digits+=c;
    return digits;
}
// Waits for the window's next refresh to show `wanted` in `control`.
static bool Shows(HWND control, const std::wstring& wanted) {
    for(int i=0; i<200 && Text(control)!=wanted; ++i) Sleep(10);
    return Text(control)==wanted;
}

// The step's name, its files as a grouped count, the bar and the percentage,
// from InstallerSteps alone.
static void TestView() {
    std::vector<std::wstring> names;
    for(const auto& step:InstallerSteps) names.push_back(std::wstring(L"Step ")+step.english);
    CHECK(Digits(sf4e::launcher::GroupedCount(1234567))==L"1234567" && sf4e::launcher::GroupedCount(12)==L"12");
    CHECK(sf4e::launcher::GroupedCount(0)==L"0");
    const auto view=sf4e::launcher::ViewInstallProgress(names,UpdateStage::Comparing,412,4756,ProgressWindow::Range);
    CHECK(view.step==names[1]);
    CHECK(view.count==sf4e::launcher::GroupedCount(412)+L" / "+sf4e::launcher::GroupedCount(4756));
    CHECK(view.position==sf4e::launcher::InstallerBarPosition(UpdateStage::Comparing,412,4756,ProgressWindow::Range));
    CHECK(view.percent==std::to_wstring(view.position*100/ProgressWindow::Range)+L"%");
    // A step whose total is not known yet shows no count; the last step done is the whole bar.
    CHECK(sf4e::launcher::ViewInstallProgress(names,UpdateStage::Preparing,0,0,ProgressWindow::Range).count.empty());
    const auto finished=sf4e::launcher::ViewInstallProgress(names,UpdateStage::Confirming,9,9,ProgressWindow::Range);
    CHECK(finished.position==ProgressWindow::Range && finished.percent==L"100%");
    // A count past its total shows the total; a launcher step shows nothing.
    CHECK(sf4e::launcher::ViewInstallProgress(names,UpdateStage::Replacing,12,10,ProgressWindow::Range).count.find(L"10 / 10")!=std::wstring::npos);
    CHECK(sf4e::launcher::ViewInstallProgress(names,UpdateStage::Downloading,5,10,ProgressWindow::Range).step.empty());
}

// A startup timeout stops a worker still behind its gate, so it cannot create
// a late window. Both successful and failed starts own no handles after
// closing; a normal close joins before destroying the owner.
static void TestLifetime() {
    ProgressWindow::Options options;
    options.visible=false; options.createThread=TrackProgressThread;
    options.timeout=0; options.startupGate=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    CHECK(options.startupGate!=nullptr);
    {
        ProgressWindow delayed(L"Installing",options);
        CHECK(delayed.Control(ProgressWindow::Part::Bar)==nullptr && progressThread!=nullptr && WaitForSingleObject(progressThread,0)==WAIT_OBJECT_0);
        SetEvent(options.startupGate);
        CHECK(delayed.Control(ProgressWindow::Part::Bar)==nullptr);
    }
    CloseHandle(progressThread); progressThread=nullptr;
    CloseHandle(options.startupGate); options.startupGate=nullptr;
    options.createThread=FailProgressThread;
    { ProgressWindow failed(L"Installing",options); CHECK(failed.Control(ProgressWindow::Part::Bar)==nullptr); }
    options.createThread=TrackProgressThread; options.timeout=5000;
    HWND bar=nullptr;
    {
        ProgressWindow normal(L"Installing",options);
        bar=normal.Control(ProgressWindow::Part::Bar);
        CHECK(bar!=nullptr && IsWindow(bar) && progressThread!=nullptr);
        CHECK(SendMessageW(bar,PBM_GETRANGE,FALSE,0)==ProgressWindow::Range);
        normal.Report(UpdateStage::Confirming,1,1);
    }
    CHECK(!IsWindow(bar) && WaitForSingleObject(progressThread,0)==WAIT_OBJECT_0);
    CloseHandle(progressThread); progressThread=nullptr;
}

// The headline from the start; the step, count, bar and percentage of the
// latest report from any thread, within a refresh.
static void TestReports() {
    ProgressWindow::Options options;
    options.visible=false;
    options.headline=L"Installing the update. It opens again when it is done.";
    for(std::size_t i=0; i<InstallerStepCount; ++i) options.stages.push_back(L"Translated step "+std::to_wstring(i));
    ProgressWindow window(L"SF4 Ember Netplay",options);
    const auto part=[&](ProgressWindow::Part which){ return window.Control(which); };
    CHECK(part(ProgressWindow::Part::Bar)!=nullptr);
    CHECK(Text(part(ProgressWindow::Part::Headline))==options.headline);
    // Before the first report, while the launcher closes or through a
    // recovery, which counts nothing, no step or percentage is shown.
    CHECK(Text(part(ProgressWindow::Part::Step)).empty() && Text(part(ProgressWindow::Part::Percent)).empty());
    // Thousands of reports, as a hashing pass sends, leave only the last shown.
    for(std::uint64_t done=1; done<=4756; ++done) window.Report(UpdateStage::Comparing,done,4756);
    window.Report(UpdateStage::Comparing,412,4756);
    const auto view=sf4e::launcher::ViewInstallProgress(options.stages,UpdateStage::Comparing,412,4756,ProgressWindow::Range);
    CHECK(Shows(part(ProgressWindow::Part::Count),view.count));
    CHECK(Text(part(ProgressWindow::Part::Step))==L"Translated step 1" && Text(part(ProgressWindow::Part::Percent))==view.percent);
    CHECK(static_cast<int>(SendMessageW(part(ProgressWindow::Part::Bar),PBM_GETPOS,0,0))==view.position);
    window.Report(UpdateStage::Replacing,0,0);
    CHECK(Shows(part(ProgressWindow::Part::Step),L"Translated step 3") && Text(part(ProgressWindow::Part::Count)).empty());
}

// Without one name per step or one text per notice, as from a launcher before
// -Stages or -Notices, the window shows their English texts.
static void TestEnglishNames() {
    ProgressWindow::Options options;
    options.visible=false;
    options.stages={L"Only one"};
    options.notices={L"Only one"};
    ProgressWindow window(L"SF4 Ember Netplay",options);
    window.Report(UpdateStage::BackingUp,1,19);
    CHECK(Shows(window.Control(ProgressWindow::Part::Step),InstallerSteps[2].english));
    window.Hear(InstallActivity::Restoring,{});
    CHECK(Shows(window.Control(ProgressWindow::Part::Step),sf4e::launcher::UpdaterNotices[2].english));
}

// The notices' texts, the clock, and what each phase shows.
static void TestNoticeText() {
    CHECK(sf4e::launcher::FillNotice(L"Waiting for {0}...",L"sf4-net.exe")==L"Waiting for sf4-net.exe...");
    CHECK(sf4e::launcher::FillNotice(L"No value",L"x")==L"No value");
    CHECK(sf4e::launcher::ElapsedText(0)==L"0:00" && sf4e::launcher::ElapsedText(42999)==L"0:42");
    CHECK(sf4e::launcher::ElapsedText(725000)==L"12:05" && sf4e::launcher::ElapsedText(3729000)==L"1:02:09");
    const std::vector<std::wstring> stages(InstallerStepCount,L"Step"), notices=sf4e::launcher::NoticeTexts({});
    using sf4e::launcher::ViewPhase;
    CHECK(ViewPhase(sf4e::launcher::WindowPhase{},stages,notices,ProgressWindow::Range).step.empty());
    const auto restoring=ViewPhase(UpdaterNotice::Restoring,stages,notices,ProgressWindow::Range);
    CHECK(restoring.step==notices[2] && restoring.count.empty() && restoring.percent.empty() && restoring.position==0);
    const auto step=ViewPhase(sf4e::launcher::StepReport{UpdateStage::Comparing,1,2},stages,notices,ProgressWindow::Range);
    CHECK(step.step==L"Step" && !step.count.empty() && !step.percent.empty());
}

// The phase and a wait for a file are separate: a wait shows beside the
// phase and ending it leaves the phase as it was; a restoration holds the
// phase until the next counted step, as a recovery before an install.
static void TestPhaseAndWait() {
    ProgressWindow::Options options;
    options.visible=false;
    for(std::size_t i=0; i<sf4e::launcher::UpdaterNoticeCount; ++i) options.notices.push_back(L"Notice "+std::to_wstring(i)+L" {0}");
    ProgressWindow window(L"SF4 Ember Netplay",options);
    const auto part=[&](ProgressWindow::Part which){ return window.Control(which); };
    // Restoration, then a wait within it, then the restoration goes on.
    window.Hear(InstallActivity::Restoring,{});
    CHECK(Shows(part(ProgressWindow::Part::Step),L"Notice 2 ") && Text(part(ProgressWindow::Part::Percent)).empty());
    window.Hear(InstallActivity::WaitingForFile,L"C:\\Games\\Ember\\sf4-net.exe");
    CHECK(Shows(part(ProgressWindow::Part::Wait),L"Notice 1 sf4-net.exe") && Text(part(ProgressWindow::Part::Step))==L"Notice 2 ");
    window.Hear(InstallActivity::WaitEnded,{});
    CHECK(Shows(part(ProgressWindow::Part::Wait),L"") && Text(part(ProgressWindow::Part::Step))==L"Notice 2 ");
    // A pending recovery before an install: the install's first step takes over.
    window.Report(UpdateStage::Preparing,0,10);
    CHECK(Shows(part(ProgressWindow::Part::Step),InstallerSteps[0].english) && Text(part(ProgressWindow::Part::Count))==L"0 / 10");
}

// While a step's count does not move, as when a file is waited for, the
// window still moves: the clock goes on, the wait is named with its file,
// and the count and percentage stay where the work is.
static void TestLiveness() {
    ProgressWindow::Options options;
    options.visible=false;
    for(std::size_t i=0; i<sf4e::launcher::UpdaterNoticeCount; ++i) options.notices.push_back(L"Notice "+std::to_wstring(i)+L" {0}");
    ProgressWindow window(L"SF4 Ember Netplay",options);
    const auto part=[&](ProgressWindow::Part which){ return window.Control(which); };
    // Before the first report, while the launcher closes.
    window.Enter(UpdaterNotice::WaitingForLauncher);
    CHECK(Shows(part(ProgressWindow::Part::Step),L"Notice 0 "));
    CHECK(Text(part(ProgressWindow::Part::Elapsed)).find(L"Notice 3 0:0")==0);
    window.Report(UpdateStage::Replacing,3,19);
    window.Hear(InstallActivity::WaitingForFile,L"sf4-net.exe");
    CHECK(Shows(part(ProgressWindow::Part::Wait),L"Notice 1 sf4-net.exe"));
    const auto percent=Text(part(ProgressWindow::Part::Percent)), count=Text(part(ProgressWindow::Part::Count));
    const auto clock=Text(part(ProgressWindow::Part::Elapsed));
    bool ticked=false;
    for(int i=0; i<250 && !ticked; ++i) { Sleep(10); ticked=Text(part(ProgressWindow::Part::Elapsed))!=clock; }
    CHECK(ticked && Text(part(ProgressWindow::Part::Percent))==percent && Text(part(ProgressWindow::Part::Count))==count);
}

int main() {
    TestView();
    TestLifetime();
    TestReports();
    TestEnglishNames();
    TestNoticeText();
    TestPhaseAndWait();
    TestLiveness();
    return 0;
}
