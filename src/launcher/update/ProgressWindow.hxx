#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <utility>
#include <vector>

namespace sf4e { namespace launcher {
// A best-effort progress window: a bar, and above it the step the install is
// at when the caller names its steps. The worker owns the windows; the
// installer receives the bar only after readiness and keeps the worker alive
// until closing.
class ProgressWindow {
public:
    static constexpr int Range=1000;
    using ThreadFactory=decltype(&CreateThread);
    struct Options {
        DWORD timeout=5000;
        HANDLE startupGate=nullptr;
        ThreadFactory createThread=&CreateThread;
        bool visible=true;
        // The steps' names, in the player's language. Without them the window
        // is the bar alone, as it was before steps were shown.
        std::vector<std::wstring> stages;
    };
    explicit ProgressWindow(const wchar_t* title) : ProgressWindow(title,Options{}) {}
    ProgressWindow(const wchar_t* title, Options options) : title_(title), options_(std::move(options)) {
        ready_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(ready_ && stop_) thread_=options_.createThread(nullptr,0,Run,this,0,nullptr);
        if(thread_ && WaitForSingleObject(ready_,options_.timeout)==WAIT_OBJECT_0) window_=published_;
        else Close();
    }
    ProgressWindow(const ProgressWindow&)=delete;
    ProgressWindow& operator=(const ProgressWindow&)=delete;
    ~ProgressWindow() { Close(); }
    // The progress bar, for PBM_SETPOS.
    HWND Window() const { return window_; }
    // The step label; null without step names.
    HWND Label() const { return window_?label_:nullptr; }
    // Shows step `index` of Options::stages. Posted, so any thread may call it
    // without waiting for the window.
    void Stage(std::size_t index) const {
        if(window_ && frame_ && index<options_.stages.size()) PostMessageW(frame_,StageMessage,index,0);
    }
    void Close() noexcept {
        if(thread_) {
            SetEvent(stop_);
            WaitForSingleObject(thread_,INFINITE);
            CloseHandle(thread_);
            thread_=nullptr;
        }
        window_=nullptr;
        if(stop_) { CloseHandle(stop_); stop_=nullptr; }
        if(ready_) { CloseHandle(ready_); ready_=nullptr; }
    }
private:
    static constexpr UINT StageMessage=WM_APP+1;
    static DWORD WINAPI Run(void* parameter) {
        return static_cast<ProgressWindow*>(parameter)->Run();
    }
    // The frame around the label and the bar. Its user data is the owning
    // ProgressWindow, which outlives the worker thread and so the window.
    static LRESULT CALLBACK FrameProc(HWND window, UINT message, WPARAM w, LPARAM l) {
        if(message==WM_NCCREATE) SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        auto* self=reinterpret_cast<ProgressWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==StageMessage && self && self->label_ && w<self->options_.stages.size()) {
            SetWindowTextW(self->label_,self->options_.stages[w].c_str());
            return 0;
        }
        if(message==WM_CTLCOLORSTATIC) return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        return DefWindowProcW(window,message,w,l);
    }
    HWND CreateWindows() {
        if(options_.stages.empty()) {
            // The bar alone, as its own captioned popup.
            const int width=440, height=72;
            return CreateWindowExW(0,PROGRESS_CLASSW,title_.c_str(),WS_POPUP|WS_CAPTION|PBS_SMOOTH,
                (GetSystemMetrics(SM_CXSCREEN)-width)/2,(GetSystemMetrics(SM_CYSCREEN)-height)/2,width,height,
                nullptr,nullptr,nullptr,nullptr);
        }
        WNDCLASSW type{}; type.lpfnWndProc=FrameProc; type.hInstance=GetModuleHandleW(nullptr);
        type.hCursor=LoadCursor(nullptr,IDC_ARROW); type.hbrBackground=GetSysColorBrush(COLOR_WINDOW);
        type.lpszClassName=L"SF4EmberUpdaterProgress";
        if(!RegisterClassW(&type) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return nullptr;
        // Laid out at 96 dpi and grown with the system's, like the label's font.
        const int dpi=static_cast<int>(GetDpiForSystem());
        const auto px=[&](int value){ return MulDiv(value,dpi>0?dpi:96,96); };
        RECT outer{0,0,px(440),px(84)};
        AdjustWindowRectEx(&outer,WS_POPUP|WS_CAPTION,FALSE,0);
        const int width=outer.right-outer.left, height=outer.bottom-outer.top;
        frame_=CreateWindowExW(0,type.lpszClassName,title_.c_str(),WS_POPUP|WS_CAPTION,
            (GetSystemMetrics(SM_CXSCREEN)-width)/2,(GetSystemMetrics(SM_CYSCREEN)-height)/2,width,height,
            nullptr,nullptr,type.hInstance,this);
        if(!frame_) return nullptr;
        NONCLIENTMETRICSW metrics{}; metrics.cbSize=sizeof(metrics);
        if(SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0)) font_=CreateFontIndirectW(&metrics.lfMessageFont);
        label_=CreateWindowExW(0,L"STATIC",options_.stages.front().c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT|SS_NOPREFIX|SS_ENDELLIPSIS,
            px(16),px(14),px(408),px(22),frame_,nullptr,type.hInstance,nullptr);
        if(label_ && font_) SendMessageW(label_,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
        const HWND bar=CreateWindowExW(0,PROGRESS_CLASSW,nullptr,WS_CHILD|WS_VISIBLE|PBS_SMOOTH,
            px(16),px(44),px(408),px(24),frame_,nullptr,type.hInstance,nullptr);
        if(!bar) { DestroyWindow(frame_); frame_=nullptr; label_=nullptr; }
        return bar;
    }
    DWORD Run() noexcept {
        if(options_.startupGate) {
            const HANDLE events[]={stop_,options_.startupGate};
            if(WaitForMultipleObjects(2,events,FALSE,INFINITE)!=WAIT_OBJECT_0+1) return 0;
        }
        if(WaitForSingleObject(stop_,0)!=WAIT_TIMEOUT) return 0;
        InitCommonControls();
        const HWND window=CreateWindows();
        // What is shown and destroyed: the frame when there is one, else the bar itself.
        const HWND top=frame_?frame_:window;
        if(window) {
            SendMessageW(window,PBM_SETRANGE32,0,Range);
            if(options_.visible && WaitForSingleObject(stop_,0)==WAIT_TIMEOUT) {
                // A hidden process's first ShowWindow honors its startup info.
                ShowWindow(top,SW_SHOWNORMAL);
                ShowWindow(top,SW_SHOWNORMAL);
                SetForegroundWindow(top);
            }
        }
        published_=window;
        SetEvent(ready_);
        if(window) {
            bool running=true;
            while(running && MsgWaitForMultipleObjects(1,&stop_,FALSE,INFINITE,QS_ALLINPUT)==WAIT_OBJECT_0+1) {
                MSG message;
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                    if(message.message==WM_QUIT) { running=false; break; }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
            DestroyWindow(top);
        }
        if(font_) { DeleteObject(font_); font_=nullptr; }
        return 0;
    }
    std::wstring title_;
    Options options_;
    HANDLE ready_=nullptr, stop_=nullptr, thread_=nullptr;
    HWND published_=nullptr, window_=nullptr, frame_=nullptr, label_=nullptr;
    HFONT font_=nullptr;
};
} }
