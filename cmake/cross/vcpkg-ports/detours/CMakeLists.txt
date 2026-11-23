cmake_minimum_required(VERSION 3.16)
project(detours CXX)
# The sources the nmake Makefile of v4.0.1 builds; each disol*.cpp is empty
# for the other architectures.
add_library(detours STATIC src/detours.cpp src/modules.cpp src/disasm.cpp src/image.cpp src/creatwth.cpp
    src/disolx86.cpp src/disolx64.cpp src/disolia64.cpp src/disolarm.cpp src/disolarm64.cpp)
target_include_directories(detours PRIVATE src)
target_compile_definitions(detours PRIVATE WIN32_LEAN_AND_MEAN)
target_compile_options(detours PRIVATE /GS-)
install(TARGETS detours ARCHIVE DESTINATION lib)
install(FILES src/detours.h src/detver.h DESTINATION include/detours)
