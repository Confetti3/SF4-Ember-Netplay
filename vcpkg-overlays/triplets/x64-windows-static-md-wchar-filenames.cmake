# SFV Ember's packages (x64-windows-static-md: static libraries, the DLL CRT)
# with spdlog opening log files by wide name. The library's sinks take
# std::wstring paths (platform/VideoLinkServe.cxx), as SF4's x86 triplet allows;
# the spdlog port reads this option only from a triplet, because it changes
# spdlog's file name type instead of adding to it.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(SPDLOG_WCHAR_FILENAMES ON)

# The mod loads as an import of the game's exe, which finds an msvcp140.dll in
# the game's folder before System32's; an older one faults the constexpr
# std::mutex of Visual Studio 2022 17.10 and later. The packages keep the
# runtime in a mutex's construction, as SFV Ember's own CMake does for what it
# compiles. vcpkg takes C++ flags only together with C flags.
set(VCPKG_CXX_FLAGS "/D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
set(VCPKG_C_FLAGS "")

# The build scripts supply the x64 MSVC environment, as for the x86 triplet.
# This also avoids older vcpkg discovery rejecting VS 18 toolsets.
set(VCPKG_LOAD_VCVARS_ENV OFF)
set(VCPKG_ENV_PASSTHROUGH PATH INCLUDE LIB LIBPATH)
