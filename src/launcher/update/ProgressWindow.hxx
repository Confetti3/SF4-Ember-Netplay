#pragma once
#include <windows.h>
#include <commctrl.h>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include "UpdaterArguments.hxx"

namespace sf4e { namespace launcher {
// A count as the player's Windows writes whole numbers, "4,756" or "4 756".
inline std::wstring GroupedCount(std::uint64_t value) {
    const std::wstring digits=std::to_wstring(value);
    wchar_t grouped[64]={}, decimal[8]={};
    if(GetNumberFormatEx(LOCALE_NAME_USER_DEFAULT,0,digits.c_str(),nullptr,grouped,64)<=0 ||
        GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT,LOCALE_SDECIMAL,decimal,8)<=0) return digits;
    // The format adds the locale's decimals; a count has none.
    std::wstring text(grouped);
    const auto point=text.rfind(decimal);
    return point==std::wstring::npos ? text : text.substr(0,point);
}
// A notice's text with its {0} filled in.
inline std::wstring FillNotice(std::wstring text, const std::wstring& value) {
    const auto at=text.find(L"{0}");
    return at==std::wstring::npos ? text : text.replace(at,3,value);
}
// Time taken so far as a clock shows it: "0:42", "12:05", "1:02:09".
inline std::wstring ElapsedText(std::uint64_t milliseconds) {
    const unsigned long long seconds=milliseconds/1000, minutes=seconds/60, hours=minutes/60;
    wchar_t text[32]={};
    if(hours) swprintf_s(text,L"%llu:%02llu:%02llu",hours,minutes%60,seconds%60);
    else swprintf_s(text,L"%llu:%02llu",minutes,seconds%60);
    return text;
}
// One counted step as Progress reports it.
struct StepReport { UpdateStage stage=UpdateStage::Preparing; std::uint64_t done=0, total=0; };
// The phase the window shows: none yet, a counted installer step, or a phase
// no count measures (waiting for the launcher to close, restoring the prior
// files), named by its notice. A wait for a file is not a phase: it is shown
// beside whichever phase it happens in.
using WindowPhase = std::variant<std::monostate, StepReport, UpdaterNotice>;
// What the window shows of the installer's report: the step's name, how many
// of its files are done ("412 / 4,756", nothing while its total is unknown),
// the bar's position and the whole install's percentage.
struct InstallProgressView {
    std::wstring step, count, percent;
    int position=0;
};
inline InstallProgressView ViewInstallProgress(const std::vector<std::wstring>& names, UpdateStage stage, std::uint64_t done, std::uint64_t total, int range) {
    InstallProgressView view;
    const int step=InstallerStepOf(stage);
    if(step<0 || static_cast<std::size_t>(step)>=names.size()) return view;
    view.step=names[static_cast<std::size_t>(step)];
    if(total) view.count=GroupedCount(done<total?done:total)+L" / "+GroupedCount(total);
    view.position=InstallerBarPosition(stage,done,total,range);
    view.percent=std::to_wstring(view.position*100/range)+L"%";
    return view;
}
// What the window shows of a phase: a step with its counts, or an uncounted
// phase's notice alone over an empty bar.
inline InstallProgressView ViewPhase(const WindowPhase& phase, const std::vector<std::wstring>& stages, const std::vector<std::wstring>& notices, int range) {
    if(const auto* step=std::get_if<StepReport>(&phase)) return ViewInstallProgress(stages,step->stage,step->done,step->total,range);
    InstallProgressView view;
    if(const auto* notice=std::get_if<UpdaterNotice>(&phase)) view.step=FillNotice(notices[static_cast<std::size_t>(*notice)],{});
    return view;
}

// The Updater's window: what is happening and that Ember opens again (the
// headline, UpdaterHeadline), then its phase (WindowPhase: the step the
// install is at with its count, and the bar with the whole install's
// percentage, or an uncounted phase), and a line with the file it waits for,
// if any, beside the time taken so far. The phase and the wait are reported
// separately from any thread (Report, Enter, Hear); the window's own thread
// shows the latest ten times a second
// and the clock moves every second, so the window never looks stopped, even
// while a count does not move, and is never flooded. The worker thread owns
// the windows; the constructor waits for them to be ready, and closing joins
// the worker before the owner goes.
class ProgressWindow {
public:
    static constexpr int Range=1000;
    using ThreadFactory=decltype(&CreateThread);
    struct Options {
        DWORD timeout=5000;
        HANDLE startupGate=nullptr;
        ThreadFactory createThread=&CreateThread;
        bool visible=true;
        // UpdaterHeadline: up to three lines.
        std::wstring headline;
        // In the player's language: the steps' names in InstallerSteps' order,
        // and the notices in UpdaterNotices' order. Their English texts when
        // either does not hold one per entry.
        std::vector<std::wstring> stages, notices;
        // For tests: hears each phase as it is set, on the thread setting it.
        std::function<void(const WindowPhase&)> entered;
    };
    enum class Part { Headline, Step, Count, Bar, Percent, Wait, Elapsed };
    static constexpr int Parts=7;
    ProgressWindow(const wchar_t* title, Options options) : title_(title), options_(std::move(options)) {
        if(options_.stages.size()!=InstallerStepCount) {
            options_.stages.clear();
            for(const auto& step:InstallerSteps) options_.stages.push_back(step.english);
        }
        options_.notices=NoticeTexts(options_.notices);
        ready_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(ready_ && stop_) thread_=options_.createThread(nullptr,0,Run,this,0,nullptr);
        if(thread_ && WaitForSingleObject(ready_,options_.timeout)==WAIT_OBJECT_0) open_=published_;
        else Close();
    }
    ProgressWindow(const ProgressWindow&)=delete;
    ProgressWindow& operator=(const ProgressWindow&)=delete;
    ~ProgressWindow() { Close(); }
    // One of the window's controls; null when the window did not open.
    HWND Control(Part part) const { return open_?parts_[static_cast<int>(part)]:nullptr; }
    // Any thread may call these three. The installer's latest report, a
    // Progress call: the phase becomes that step.
    void Report(UpdateStage stage, std::uint64_t done, std::uint64_t total) { Set(StepReport{stage,done,total}); }
    // An uncounted phase: WaitingForLauncher, Recovering or Restoring.
    void Enter(UpdaterNotice phase) { Set(phase); }
    // An Activity call: Recovering and Restoring enter those phases; a wait
    // for a file shows the file until it ends, whatever the phase.
    void Hear(InstallActivity activity, const std::filesystem::path& file) {
        if(activity==InstallActivity::Recovering) { Enter(UpdaterNotice::Recovering); return; }
        if(activity==InstallActivity::Restoring) { Enter(UpdaterNotice::Restoring); return; }
        std::lock_guard<std::mutex> lock(mutex_);
        if(activity==InstallActivity::WaitingForFile) waitingFor_=file.filename().wstring();
        else waitingFor_.reset();
    }
    void Close() noexcept {
        if(thread_) {
            SetEvent(stop_);
            WaitForSingleObject(thread_,INFINITE);
            CloseHandle(thread_);
            thread_=nullptr;
        }
        open_=false;
        if(stop_) { CloseHandle(stop_); stop_=nullptr; }
        if(ready_) { CloseHandle(ready_); ready_=nullptr; }
    }
private:
    static constexpr UINT_PTR RefreshTimer=1;
    static constexpr UINT RefreshMilliseconds=100;
    static DWORD WINAPI Run(void* parameter) {
        return static_cast<ProgressWindow*>(parameter)->Run();
    }
    // The frame around the controls. Its user data is the owning
    // ProgressWindow, which outlives the worker thread and so the window.
    static LRESULT CALLBACK FrameProc(HWND window, UINT message, WPARAM w, LPARAM l) {
        if(message==WM_NCCREATE) SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        auto* self=reinterpret_cast<ProgressWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_TIMER && w==RefreshTimer && self) { self->Refresh(); return 0; }
        if(message==WM_CTLCOLORSTATIC) return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        return DefWindowProcW(window,message,w,l);
    }
    HWND At(Part part) const { return parts_[static_cast<int>(part)]; }
    void Set(WindowPhase phase) {
        { std::lock_guard<std::mutex> lock(mutex_); phase_=phase; }
        if(options_.entered) options_.entered(phase);
    }
    // Sets a control's text only when it changed, so nothing flickers.
    static void Show(HWND control, const std::wstring& text) {
        wchar_t shown[256]={};
        GetWindowTextW(control,shown,256);
        if(text!=shown) SetWindowTextW(control,text.c_str());
    }
    std::wstring Notice(UpdaterNotice notice, const std::wstring& value) const {
        return FillNotice(options_.notices[static_cast<std::size_t>(notice)],value);
    }
    void Refresh() {
        WindowPhase phase;
        std::optional<std::wstring> waitingFor;
        { std::lock_guard<std::mutex> lock(mutex_); phase=phase_; waitingFor=waitingFor_; }
        Show(At(Part::Elapsed),Notice(UpdaterNotice::Elapsed,ElapsedText(GetTickCount64()-started_)));
        Show(At(Part::Wait),waitingFor ? Notice(UpdaterNotice::WaitingForFile,*waitingFor) : std::wstring());
        const auto view=ViewPhase(phase,options_.stages,options_.notices,Range);
        Show(At(Part::Step),view.step);
        Show(At(Part::Count),view.count);
        Show(At(Part::Percent),view.percent);
        SendMessageW(At(Part::Bar),PBM_SETPOS,view.position,0);
    }
    // Laid out at 96 dpi and grown with the system's, like the font.
    bool CreateWindows() {
        WNDCLASSW type{}; type.lpfnWndProc=FrameProc; type.hInstance=GetModuleHandleW(nullptr);
        type.hCursor=LoadCursor(nullptr,IDC_ARROW); type.hbrBackground=GetSysColorBrush(COLOR_WINDOW);
        type.lpszClassName=L"SF4EmberUpdaterProgress";
        if(!RegisterClassW(&type) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
        const int dpi=static_cast<int>(GetDpiForSystem());
        const auto px=[&](int value){ return MulDiv(value,dpi>0?dpi:96,96); };
        RECT outer{0,0,px(440),px(178)};
        AdjustWindowRectEx(&outer,WS_POPUP|WS_CAPTION,FALSE,0);
        const int width=outer.right-outer.left, height=outer.bottom-outer.top;
        frame_=CreateWindowExW(0,type.lpszClassName,title_.c_str(),WS_POPUP|WS_CAPTION,
            (GetSystemMetrics(SM_CXSCREEN)-width)/2,(GetSystemMetrics(SM_CYSCREEN)-height)/2,width,height,
            nullptr,nullptr,type.hInstance,this);
        if(!frame_) return false;
        NONCLIENTMETRICSW metrics{}; metrics.cbSize=sizeof(metrics);
        if(SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0)) font_=CreateFontIndirectW(&metrics.lfMessageFont);
        const auto make=[&](Part part, const wchar_t* type, const wchar_t* text, DWORD style, int x, int y, int w, int h) {
            const HWND control=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,px(x),px(y),px(w),px(h),frame_,nullptr,GetModuleHandleW(nullptr),nullptr);
            if(control && font_) SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
            parts_[static_cast<int>(part)]=control;
            return control!=nullptr;
        };
        const DWORD label=SS_NOPREFIX|SS_ENDELLIPSIS;
        // The headline (two lines and a wrap) and the wait line wrap in a longer
        // language, so a waited file's name is never cut.
        const bool made=make(Part::Headline,L"STATIC",options_.headline.c_str(),SS_LEFT|SS_NOPREFIX,16,12,408,56) &&
            make(Part::Step,L"STATIC",L"",SS_LEFT|label,16,76,318,20) &&
            make(Part::Count,L"STATIC",L"",SS_RIGHT|label,334,76,90,20) &&
            make(Part::Bar,PROGRESS_CLASSW,nullptr,PBS_SMOOTH,16,102,356,22) &&
            make(Part::Percent,L"STATIC",L"",SS_RIGHT|SS_CENTERIMAGE|label,376,102,48,22) &&
            make(Part::Wait,L"STATIC",L"",SS_LEFT|SS_NOPREFIX,16,134,300,36) &&
            make(Part::Elapsed,L"STATIC",L"",SS_RIGHT|label,316,134,108,20);
        if(!made) { DestroyWindow(frame_); frame_=nullptr; return false; }
        SendMessageW(At(Part::Bar),PBM_SETRANGE32,0,Range);
        SetTimer(frame_,RefreshTimer,RefreshMilliseconds,nullptr);
        Refresh();
        return true;
    }
    DWORD Run() noexcept {
        if(options_.startupGate) {
            const HANDLE events[]={stop_,options_.startupGate};
            if(WaitForMultipleObjects(2,events,FALSE,INFINITE)!=WAIT_OBJECT_0+1) return 0;
        }
        if(WaitForSingleObject(stop_,0)!=WAIT_TIMEOUT) return 0;
        InitCommonControls();
        const bool created=CreateWindows();
        if(created && options_.visible && WaitForSingleObject(stop_,0)==WAIT_TIMEOUT) {
            // A hidden process's first ShowWindow honors its startup info.
            ShowWindow(frame_,SW_SHOWNORMAL);
            ShowWindow(frame_,SW_SHOWNORMAL);
            SetForegroundWindow(frame_);
        }
        published_=created;
        SetEvent(ready_);
        if(created) {
            bool running=true;
            while(running && MsgWaitForMultipleObjects(1,&stop_,FALSE,INFINITE,QS_ALLINPUT)==WAIT_OBJECT_0+1) {
                MSG message;
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                    if(message.message==WM_QUIT) { running=false; break; }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            DestroyWindow(frame_);
        }
        if(font_) { DeleteObject(font_); font_=nullptr; }
        return 0;
    }
    std::wstring title_;
    Options options_;
    const ULONGLONG started_=GetTickCount64();
    HANDLE ready_=nullptr, stop_=nullptr, thread_=nullptr;
    bool published_=false, open_=false;
    HWND frame_=nullptr;
    HWND parts_[Parts]={};
    HFONT font_=nullptr;
    std::mutex mutex_;
    WindowPhase phase_;
    std::optional<std::wstring> waitingFor_;
};
} }
