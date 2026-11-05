# SFV Ember's packages (x64-windows-static-md: static libraries, the DLL CRT)
# with spdlog opening log files by wide name. The library's sinks take
# std::wstring paths (platform/VideoLinkServe.cxx), as SF4's x86 triplet allows;
# the spdlog port reads this option only from a triplet, because it changes
# spdlog's file name type instead of adding to it.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(SPDLOG_WCHAR_FILENAMES ON)

# The build scripts supply the x64 MSVC environment, as for the x86 triplet.
# This also avoids older vcpkg discovery rejecting VS 18 toolsets.
set(VCPKG_LOAD_VCVARS_ENV OFF)
set(VCPKG_ENV_PASSTHROUGH PATH INCLUDE LIB LIBPATH)
