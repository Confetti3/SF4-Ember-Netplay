#include <windows.h>
#include "Dimps__Eva.hxx"
#include "Dimps__Platform.hxx"

using Dimps::Eva::IEmSpriteAction;
using Dimps::Eva::IEmSpriteNode;

namespace Platform = Dimps::Platform;
using Platform::Allocator;
using Platform::dDeque_0x10;
using Platform::dString;
using Platform::D3D;
using Platform::Main;
using Platform::GFxApp;
using Platform::Sound;

Allocator::__publicMethods Allocator::publicMethods;
Allocator::__staticMethods Allocator::staticMethods;
dString::__publicMethods dString::publicMethods;
dDeque_0x10::__publicMethods dDeque_0x10::publicMethods;
D3D::__privateMethods D3D::privateMethods;
D3D::__publicMethods D3D::publicMethods;
D3D::__staticMethods D3D::staticMethods;
Main::__privateMethods Main::privateMethods;
Main::__publicMethods Main::publicMethods;
Main::__staticMethods Main::staticMethods;
GFxApp::__publicMethods GFxApp::publicMethods;
GFxApp::__staticMethods GFxApp::staticMethods;
Sound::__staticMethods Sound::staticMethods;

void Platform::Locate(HMODULE peRoot) {
    Allocator::Locate(peRoot);
    dDeque_0x10::Locate(peRoot);
    dString::Locate(peRoot);
    D3D::Locate(peRoot);
    Main::Locate(peRoot);
    GFxApp::Locate(peRoot);
    Sound::Locate(peRoot);
}

void Allocator::Locate(HMODULE peRoot) {
    unsigned int peRootOffset = (unsigned int)peRoot;
    *(PVOID*)&publicMethods.Allocate = (PVOID)(peRootOffset + 0x2b3240);
    *(PVOID*)&staticMethods.GetSingleton = (PVOID)(peRootOffset + 0x2b2fd0);
}

void dDeque_0x10::Locate(HMODULE peRoot) {
    unsigned int peRootOffset = (unsigned int)peRoot;
    *(PVOID*)&publicMethods.clear = (PVOID)(peRootOffset + 0x29bd20);
    *(PVOID*)&publicMethods.push_back = (PVOID)(peRootOffset + 0x18a540);
}

void dString::Locate(HMODULE peRoot) {
    unsigned int peRootOffset = (unsigned int)peRoot;
    *(PVOID*)&publicMethods.assign = (PVOID)(peRootOffset + 0x4890);
}

void D3D::Locate(HMODULE peRoot) {
    unsigned int peRootOffset = (unsigned int)peRoot;
    *(PVOID*)&privateMethods.Destroy = (PVOID)(peRootOffset + 0x372820);
    *(PVOID*)&privateMethods.Reset = (PVOID)(peRootOffset + 0x3725b0);
    *(PVOID*)&privateMethods.RunScene_Render = (PVOID)(peRootOffset + 0x370cc0);
    staticMethods.GetSingleton = (D3D * (WINAPI*)())(peRootOffset + 0x371180);
}

void Main::Locate(HMODULE peRoot) {
    unsigned int peRootOffset = (unsigned int)peRoot;
    *(PVOID*)&publicMethods.Initialize = (PVOID)(peRootOffset + 0x37e3e0);
    *(PVOID*)&publicMethods.Destroy = (PVOID)(peRootOffset + 0x37e6c0);
    staticMethods.GetSingleton = (Main*(*)())(peRootOffset + 0x37eab0);
    staticMethods.RunWindowFunc = (void(WINAPI*)(Main*, HWND, UINT, WPARAM, LPARAM))(peRootOffset + 0x37eb20);
}

Main::Win32_WindowData** Main::GetWindowData(Main* m) {
    return (Win32_WindowData**)((unsigned int)m + 0x490);
}

void GFxApp::Locate(HMODULE peRoot) {
    unsigned int peRootOffset = (unsigned int)peRoot;
    *(PVOID*)&publicMethods.GetFrameDelta_Float = (PVOID)(peRootOffset + 0x2b65a0);
    *(PVOID*)&publicMethods.SetFrameDelta_Float = (PVOID)(peRootOffset + 0x2b66e0);
    staticMethods.GetSingleton = (GFxApp*(*)())(peRootOffset + 0x2b7660);
}

GFxApp::ObjectPool<IEmSpriteAction>* GFxApp::GetActionPool(GFxApp* a) {
    return (GFxApp::ObjectPool<IEmSpriteAction>*)((unsigned int)a + 0x4);
}

GFxApp::ObjectPool<IEmSpriteNode>* GFxApp::GetNodePool(GFxApp* a) {
    return (GFxApp::ObjectPool<IEmSpriteNode>*)((unsigned int)a + 0x24);
}

void Sound::Locate(HMODULE peRoot) {
    unsigned int peRootOffset = (unsigned int)peRoot;
    staticMethods.GetNewPlayerHandle = (uint32_t(*)())(peRootOffset + 0x275390);
}