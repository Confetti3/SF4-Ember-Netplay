# SF4 Ember's Windows build on a Linux build PC (scripts/build-current.ps1 on Linux, docs/release/LAPTOP_BUILD.md):
# clang-cl, lld-link, llvm-lib, llvm-rc and llvm-mt against the Microsoft CRT and Windows SDK that xwin splats.
# clang-cl-x86.cmake (the game's modules, Launcher and tests) and clang-cl-x64.cmake (the Discord companion and its
# test fixture) set SF4E_CROSS_ARCH and include this file. SF4E_XWIN names the splat and SF4E_LLVM_BIN the LLVM
# tools. vcpkg's ports are built with it too: build-current.ps1's triplet chainloads clang-cl-x86.cmake.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
# The splat has no debug CRT libraries, and try_compile defaults to Debug.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

if(NOT DEFINED ENV{SF4E_XWIN} OR NOT DEFINED ENV{SF4E_LLVM_BIN})
    message(FATAL_ERROR "cmake/cross needs SF4E_XWIN (the xwin splat) and SF4E_LLVM_BIN (the LLVM tools)")
endif()
set(_xwin "$ENV{SF4E_XWIN}")
set(_llvm "$ENV{SF4E_LLVM_BIN}")

if(SF4E_CROSS_ARCH STREQUAL "x86")
    set(_triple i686-pc-windows-msvc)
    set(_libarch x86)
    set(CMAKE_SYSTEM_PROCESSOR X86)
elseif(SF4E_CROSS_ARCH STREQUAL "x64")
    set(_triple x86_64-pc-windows-msvc)
    set(_libarch x86_64)
    set(CMAKE_SYSTEM_PROCESSOR AMD64)
else()
    message(FATAL_ERROR "Include clang-cl.cmake through clang-cl-x86.cmake or clang-cl-x64.cmake")
endif()

set(CMAKE_C_COMPILER "${_llvm}/clang-cl")
set(CMAKE_CXX_COMPILER "${_llvm}/clang-cl")
set(CMAKE_LINKER "${_llvm}/lld-link")
set(CMAKE_AR "${_llvm}/llvm-lib")
set(CMAKE_RC_COMPILER "${_llvm}/llvm-rc")
set(CMAKE_MT "${_llvm}/llvm-mt")

# _MSC_VER is the toolset of the CRT and STL headers in the splat (crtversion.h), as MSVC's own would be: the
# Launcher requires at least that runtime (RuntimeIsCurrent), and clang-cl's default (19.33) is older.
file(STRINGS "${_xwin}/crt/include/crtversion.h" _crt_minor REGEX "^#define _VC_CRT_MINOR_VERSION [0-9]+")
string(REGEX MATCH "[0-9]+$" _crt_minor "${_crt_minor}")
if(NOT _crt_minor)
    message(FATAL_ERROR "No _VC_CRT_MINOR_VERSION in ${_xwin}/crt/include/crtversion.h")
endif()
set(_msc "-fms-compatibility-version=19.${_crt_minor}")

set(_inc "/imsvc ${_xwin}/crt/include /imsvc ${_xwin}/sdk/include/ucrt /imsvc ${_xwin}/sdk/include/um /imsvc ${_xwin}/sdk/include/shared /imsvc ${_xwin}/sdk/include/winrt")
# The toolchain's own folders as neutral names wherever a path of theirs is compiled in (__FILE__ in a header).
set(_maps "/clang:-ffile-prefix-map=${_xwin}=/xwin /clang:-ffile-prefix-map=${_llvm}=/llvm")
# Warnings off: the sources are written against MSVC's warnings, which the desktop build reports; clang's differ
# and change nothing that is compiled.
set(CMAKE_C_FLAGS_INIT "--target=${_triple} ${_msc} /w ${_inc} ${_maps}")
set(CMAKE_CXX_FLAGS_INIT "--target=${_triple} ${_msc} /w ${_inc} ${_maps}")
if(DEFINED VCPKG_CRT_LINKAGE)
    # Inside a vcpkg port build: the release flags of vcpkg's own Windows toolchain, which the desktop's ports use,
    # including their debug information, so Breakpad symbols can be made for the DLLs Sidecar loads
    # (New-EmberSymbols). A port's compiles go through the build's compiler cache, as the project's do.
    string(APPEND CMAKE_C_FLAGS_INIT " /utf-8")
    string(APPEND CMAKE_CXX_FLAGS_INIT " /utf-8")
    set(CMAKE_C_FLAGS_RELEASE_INIT "/O2 /Oi /Gy /DNDEBUG /Z7")
    set(CMAKE_CXX_FLAGS_RELEASE_INIT "/O2 /Oi /Gy /DNDEBUG /Z7")
    foreach(_kind EXE SHARED MODULE)
        set(CMAKE_${_kind}_LINKER_FLAGS_RELEASE_INIT "/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF")
    endforeach()
    if(DEFINED ENV{SF4E_COMPILER_LAUNCHER})
        set(CMAKE_C_COMPILER_LAUNCHER "$ENV{SF4E_COMPILER_LAUNCHER}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "$ENV{SF4E_COMPILER_LAUNCHER}")
    endif()
endif()
set(_libs "/libpath:${_xwin}/crt/lib/${_libarch} /libpath:${_xwin}/sdk/lib/ucrt/${_libarch} /libpath:${_xwin}/sdk/lib/um/${_libarch}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_libs}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_libs}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_libs}")
set(CMAKE_RC_FLAGS_INIT "-DWIN32 /I ${_xwin}/sdk/include/um /I ${_xwin}/sdk/include/shared /I ${_xwin}/sdk/include/ucrt /I ${_xwin}/crt/include")
