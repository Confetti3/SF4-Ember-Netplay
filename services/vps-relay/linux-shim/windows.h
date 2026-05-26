#pragma once

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef WINAPI
#define WINAPI
#endif

typedef std::uint8_t BYTE;
typedef std::uint16_t WORD;
typedef std::uint32_t DWORD;
typedef std::uint64_t UINT64;
typedef std::uint32_t UINT;
typedef int BOOL;
typedef void* HMODULE;
typedef void* HWND;
typedef std::uintptr_t WPARAM;
typedef std::intptr_t LPARAM;

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif
