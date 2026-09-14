#include "../platform/ApplicationServices.hxx"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#define CHECK(c) do { if (!(c)) { std::cerr << "Failure at " << __LINE__ << '\n'; std::exit(1); } } while (false)
int main() {
    namespace fs = std::filesystem;
    using namespace sf4e::platform;
    const auto root = fs::temp_directory_path()/(L"ember-services-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    {
        ApplicationServices service(root.wstring()); DiagnosticsView view;
        CHECK(!service.Request(ServiceAction::None));
        view.probeState=4; view.probeFailure=8;
        CHECK(DescribeDiagnostics(view).find("Check rejection: Seat or table revision changed")!=std::string::npos);
        view.probeFailure=999;
        CHECK(DescribeDiagnostics(view).find("Check rejection: Unavailable")!=std::string::npos);
        view.probeState=0; view.probeFailure=0;
        for (int i=0;i<40;++i) { view.room=i%5; service.Observe(view); }
        CHECK(service.Snapshot().connectionHistory.size()==16);
        view.probeState=2;view.probeRoute=1;view.p95Us=45000;view.replies=96;view.missed=4;view.directLinks=3;
        view.room=999; CHECK(DescribeDiagnostics(view).find("Room: Unavailable")!=std::string::npos);
        CHECK(service.Request(ServiceAction::ExportDiagnostics,view));
        const auto deadline=GetTickCount64()+5000;
        while(service.Snapshot().pending && GetTickCount64()<deadline) Sleep(1);
        CHECK(!service.Snapshot().pending);
        std::ifstream input(root/L"ember-diagnostics.txt");
        const std::string contents{std::istreambuf_iterator<char>(input),{}};
        CHECK(contents.size()<8192 && contents.find("Ping: Unavailable")!=std::string::npos);
        CHECK(contents.find("Room: Unavailable")!=std::string::npos);
        CHECK(contents.find("Measured route: Direct")!=std::string::npos);
        CHECK(contents.find("Replies/missed: 96/4")!=std::string::npos);
        CHECK(contents.find("Gameplay direct/relay links: 3/0")!=std::string::npos);
        CHECK(contents.find("CPU timing: unavailable")!=std::string::npos);
        service.Observe(view);
        const auto history=service.Snapshot().connectionHistory;
        view.performanceEnabled=true; view.selectedDelay=2;
        view.timings[0]={600,4,3.5,31.0}; view.timings[1]={600,0,0.2,1.0};
        view.rollbackCallbacks=12; view.predictionStalls=3; view.predictionSkippedFrames=5;
        service.Observe(view);
        CHECK(service.Snapshot().connectionHistory==history);
        CHECK(service.Request(ServiceAction::ExportDiagnostics,view));
        const auto performanceDeadline=GetTickCount64()+5000;
        while(service.Snapshot().pending&&GetTickCount64()<performanceDeadline)Sleep(1);
        CHECK(!service.Snapshot().pending);
        std::ifstream performanceInput(root/L"ember-diagnostics.txt");
        const std::string performance{std::istreambuf_iterator<char>(performanceInput),{}};
        CHECK(performance.find("not displayed FPS")!=std::string::npos);
        CHECK(performance.find("Outer tick: samples=600 mean_ms=3.5 max_ms=31 over_25ms=4")!=std::string::npos);
        CHECK(performance.find("Room runtime: samples=600 mean_ms=0.2")!=std::string::npos);
        CHECK(performance.find("Prediction stalls: 3 | Prediction-skipped frames: 5")!=std::string::npos);
        CHECK(performance.find("Selected input delay: 2 frames")!=std::string::npos);
        // Export is constructed from this typed allowlist, never arbitrary logs/settings.
        CHECK(contents.find("invitation")==std::string::npos && contents.find("capability")==std::string::npos);
    }
    fs::remove_all(root);
    std::cout<<"Bounded readable diagnostics, unavailable state and worker export passed\n";
}
