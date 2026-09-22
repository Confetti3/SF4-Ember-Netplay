#pragma once

#include <deque>
#include <stdint.h>
#include <string>
#include <windows.h>
#include <d3d9.h>

#include "Dimps__Eva.hxx"

// These are derived from address 0x9670f0- GfxApp::Initialize is not called
// with a pointer to the various sizes that define the app, so the app uses
// its defaults at that address.
#define NUM_GFX_ACTIONS 0x44c
#define NUM_GFX_NODES 0x44c
#define NUM_GFX_BATCHNODES 0x100 
#define NUM_GFX_RENDERRELATED 0x100
#define NUM_GFX_MATRICES 0x20
#define NUM_GFX_CALLBACKS 0x100

namespace Dimps {
	namespace Platform {
		template <class T>
		struct WithReleaser {
			T* obj;
			void (T::* releaser)();
		};

		template <class T>
		struct list_entry {
			list_entry<T>* next;
			list_entry<T>* prev;
			T data;
		};

		template <class T>
		struct list {
			list_entry<T>* root;
			int numUsed;
		};

		// While SF4 seems to use std::string internally and std::string
		// pointers can actually be used to read data, they can't naively
		// be used to _write_ data. It's unclear if this is because SF4
		// uses a custom allocator for strings, because the DLL is linking
		// against a different std::string implementation that could be solved
		// with a different msvcrt, or another unidentified reason.
		// Regardless, we can hack around all of these by "de-templatizing"
		// the allocator and locating the allocator methods as if they were
		// method pointers of a string.
		struct dString : std::string {
			typedef struct __publicMethods {
				dString* (dString::* assign)(const char* s, int count);
			} __publicMethods;

			static __publicMethods publicMethods;
			static void Locate(HMODULE peRoot);
		};

		// The problems of the string also apply here to the deque,
		// but with the additional twist that there are far more
		// template specializations of the std::deque due to
		// allocating differently sized memory.
		struct Stub0x10 {
			int pad[4];
		};

		struct dDeque_0x10 : std::deque<Stub0x10> {
			typedef struct __publicMethods {
				void (dDeque_0x10::* push_back)(Stub0x10* value);
				void (dDeque_0x10::* clear)();
			} __publicMethods;

			static __publicMethods publicMethods;
			static void Locate(HMODULE peRoot);
		};

		void Locate(HMODULE peRoot);

		struct D3D
		{
			typedef struct __privateMethods {
				void(D3D::* Destroy)();
				DWORD (D3D::* Reset)();
				void (D3D::* RunScene_Render)(void* pList);
				// The FIXED frame-rate limiter, run after Present: spins until
				// periodSeconds after lastLimiterExit, then records the exit.
				int (D3D::* LimitFrame)(float frameDelta);
				// Fills the present parameters for device creation and Reset.
				void (D3D::* BuildPresentParameters)();
			} __privateMethods;

			typedef struct __publicMethods {
				// TODO
			} __publicMethods;

			typedef struct __staticMethods {
				D3D* (WINAPI* GetSingleton)();
			} __staticMethods;

			static void Locate(HMODULE peRoot);
			static __privateMethods privateMethods;
			static __publicMethods publicMethods;
			static __staticMethods staticMethods;

			// Instance values here
			void* vtable;
			HWND hFocusWindow;
			DWORD workingMem;
			DWORD deviceType;
			DWORD behaviorFlags;
			IDirect3D9* lpD3D;
			IDirect3DDevice9* lpD3DDevice;

			static unsigned long long* GetLastLimiterExit(D3D* d) {
				return (unsigned long long*)((unsigned int)d + 0x1f0); // QPC ticks
			}
			static float* GetFramePeriodSeconds(D3D* d) {
				return (float*)((unsigned int)d + 0x1f8); // 1/60 under FIXED
			}
			static D3DPRESENT_PARAMETERS* GetPresentParameters(D3D* d) {
				return (D3DPRESENT_PARAMETERS*)((unsigned int)d + 0x1c);
			}
		};

		struct Main
		{
			typedef struct __privateMethods {
				// TODO
			} __privateMethods;

			typedef struct __publicMethods {
				int (Main::* Initialize)(void*, void*, void*);
				void (Main::* Destroy)();
			} __publicMethods;

			typedef struct __staticMethods {
				Main* (*GetSingleton)();
				void (WINAPI* RunWindowFunc)(Main* lpMain, HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
			} __staticMethods;

			static void Locate(HMODULE peRoot);
			static __privateMethods privateMethods;
			static __publicMethods publicMethods;
			static __staticMethods staticMethods;

			typedef struct Win32_WindowData {
				void* field_0x0;
				HWND hWnd;
				char field_0x8[44];
			} Win32_WindowData;

			static Win32_WindowData** GetWindowData(Main* m);
		};

		struct GFxApp
		{
			template <class T>
			struct ObjectPool {
				void* myData;
				T* raw;
				T** free;
				uint8_t* useIndex;
				uint32_t max;
				uint32_t used;
				uint32_t count_0x18;
				uint32_t pad_0x1c;
			};

			typedef struct __publicMethods {
				void (GFxApp::* SetFrameDelta_Float)(float frames);
				float (GFxApp::* GetFrameDelta_Float)();
			} __publicMethods;

			typedef struct __staticMethods {
				GFxApp* (*GetSingleton)();
			} __staticMethods;

			static ObjectPool<Dimps::Eva::IEmSpriteAction>* GetActionPool(GFxApp* a);
			// static ObjectPool<void*> GetBatchPool(GFxApp* a);
			static ObjectPool<Dimps::Eva::IEmSpriteNode>* GetNodePool(GFxApp* a);
			static void Locate(HMODULE peRoot);
			static __publicMethods publicMethods;
			static __staticMethods staticMethods;
		};

		struct Allocator
		{
			typedef struct __publicMethods {
				// arg2 and arg3 are almost always 0 and -1, respectively
				void* (Allocator::* Allocate)(size_t size, DWORD arg2, DWORD arg3);
			} __publicMethods;

			typedef struct __staticMethods {
				Allocator* (*GetSingleton)();
			} __staticMethods;

			static void Locate(HMODULE peRoot);
			static __publicMethods publicMethods;
			static __staticMethods staticMethods;
		};

		struct Sound
		{
			typedef struct __staticMethods {
				uint32_t (*GetNewPlayerHandle)();
			} __staticMethods;

			static void Locate(HMODULE peRoot);
			static __staticMethods staticMethods;
		};

		template <int N>
		struct SoundObjectPoolEntry {
			uint32_t handle;
			DWORD field_0x4;
			SoundObjectPoolEntry<N>* prev;
			SoundObjectPoolEntry<N>* next;
			uint8_t data[N];
		};

		template <int N>
		struct SoundObjectPool {
			using Entry = SoundObjectPoolEntry<N>;

			SoundObjectPoolEntry<N>* inactiveHead;
			SoundObjectPoolEntry<N>* inactiveTail;
			SoundObjectPoolEntry<N>* activeHead;
			SoundObjectPoolEntry<N>* activeTail;
			uint32_t count;
			BOOL locked;
		};
	}
}