#pragma once
#include <windows.h>
#include <commctrl.h>
#include <string>

namespace sf4e { namespace launcher {
// A best-effort progress window. The worker owns its HWND; the installer
// receives it only after readiness and keeps the worker alive until closing.
class ProgressWindow {
public:
    static constexpr int Range=1000;
    using ThreadFactory=decltype(&CreateThread);
    struct Options {
        DWORD timeout=5000;
        HANDLE startupGate=nullptr;
        ThreadFactory createThread=&CreateThread;
        bool visible=true;
    };
    explicit ProgressWindow(const wchar_t* title) : ProgressWindow(title,Options{}) {}
    ProgressWindow(const wchar_t* title, Options options) : title_(title), options_(options) {
        ready_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        stop_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(ready_ && stop_) thread_=options_.createThread(nullptr,0,Run,this,0,nullptr);
        if(thread_ && WaitForSingleObject(ready_,options_.timeout)==WAIT_OBJECT_0) window_=published_;
        else Close();
    }
    ProgressWindow(const ProgressWindow&)=delete;
    ProgressWindow& operator=(const ProgressWindow&)=delete;
    ~ProgressWindow() { Close(); }
    HWND Window() const { return window_; }
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
    static DWORD WINAPI Run(void* parameter) {
        return static_cast<ProgressWindow*>(parameter)->Run();
    }
    DWORD Run() noexcept {
        if(options_.startupGate) {
            const HANDLE events[]={stop_,options_.startupGate};
            if(WaitForMultipleObjects(2,events,FALSE,INFINITE)!=WAIT_OBJECT_0+1) return 0;
        }
        if(WaitForSingleObject(stop_,0)!=WAIT_TIMEOUT) return 0;
        const int width=440, height=72;
        InitCommonControls();
        const HWND window=CreateWindowExW(0,PROGRESS_CLASSW,title_.c_str(),WS_POPUP|WS_CAPTION|PBS_SMOOTH,
            (GetSystemMetrics(SM_CXSCREEN)-width)/2,(GetSystemMetrics(SM_CYSCREEN)-height)/2,width,height,
            nullptr,nullptr,nullptr,nullptr);
        if(window) {
            SendMessageW(window,PBM_SETRANGE32,0,Range);
            if(options_.visible && WaitForSingleObject(stop_,0)==WAIT_TIMEOUT) {
                // A hidden process's first ShowWindow honors its startup info.
                ShowWindow(window,SW_SHOWNORMAL);
                ShowWindow(window,SW_SHOWNORMAL);
                SetForegroundWindow(window);
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
            DestroyWindow(window);
        }
        return 0;
    }
    std::wstring title_;
    Options options_;
    HANDLE ready_=nullptr, stop_=nullptr, thread_=nullptr;
    HWND published_=nullptr, window_=nullptr;
};
} }
