# The generator arguments of the x64 sub-builds (the Discord companion and its test fixture) for
# ExternalProject_Add. On Windows they use Visual Studio's x64 tools, from the installation the main build's
# compiler belongs to. A cross build (scripts/build-current.ps1 on Linux) uses Ninja with the x64 half of the cross
# toolchain (cmake/cross/clang-cl-x64.cmake), through the main build's compiler cache. A single-configuration
# generator puts programs in the binary folder itself; RUNTIME_DIRECTORY gives them the folder Visual Studio's
# Release configuration would.
function(sf4e_x64_subbuild_args out binary_dir)
    if(CMAKE_HOST_WIN32)
        string(REGEX REPLACE "/VC/.*$" "" vs_instance "${CMAKE_CXX_COMPILER}")
        set(args CMAKE_GENERATOR "Visual Studio 18 2026" CMAKE_GENERATOR_PLATFORM x64 CMAKE_GENERATOR_INSTANCE "${vs_instance}")
    else()
        set(cache "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_SOURCE_DIR}/cmake/cross/clang-cl-x64.cmake"
            "-DCMAKE_BUILD_TYPE:STRING=Release"
            "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY:PATH=${binary_dir}/Release")
        foreach(lang C CXX)
            if(CMAKE_${lang}_COMPILER_LAUNCHER)
                list(APPEND cache "-DCMAKE_${lang}_COMPILER_LAUNCHER:STRING=${CMAKE_${lang}_COMPILER_LAUNCHER}")
            endif()
        endforeach()
        set(args CMAKE_GENERATOR Ninja CMAKE_CACHE_ARGS ${cache})
    endif()
    set(${out} "${args}" PARENT_SCOPE)
endfunction()
