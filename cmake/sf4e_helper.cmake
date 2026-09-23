# The game and Sidecar remain x86. Cargo explicitly builds the shipped helper
# for x64 Windows, independent of the active CMake/MSVC architecture.
option(SF4E_BUILD_IROH_HELPER "Build the bundled x64 Iroh helper" ON)
if(WIN32)
    add_library(sf4e_helper_platform STATIC
        "${CMAKE_SOURCE_DIR}/src/platform/HelperProcess.cxx"
        "${CMAKE_SOURCE_DIR}/src/platform/HelperClient.cxx")
    target_compile_features(sf4e_helper_platform PUBLIC cxx_std_11)
    target_compile_definitions(sf4e_helper_platform PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(sf4e_helper_platform PUBLIC bcrypt)

    if(SF4E_BUILD_IROH_HELPER)
        find_program(SF4E_CARGO_EXECUTABLE cargo REQUIRED)
        set(SF4E_HELPER_SOURCE "${CMAKE_SOURCE_DIR}/rust/sf4-net")
        set(SF4E_HELPER_EXECUTABLE "${CMAKE_BINARY_DIR}/sf4-net.exe")
        file(GLOB_RECURSE SF4E_HELPER_RUST_SOURCES CONFIGURE_DEPENDS "${SF4E_HELPER_SOURCE}/src/*.rs")
        add_custom_command(OUTPUT "${SF4E_HELPER_EXECUTABLE}"
            COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/rust-target"
                "CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS=-C target-feature=+crt-static"
                "${SF4E_CARGO_EXECUTABLE}" build --locked --release --target x86_64-pc-windows-msvc
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_BINARY_DIR}/rust-target/x86_64-pc-windows-msvc/release/sf4-net.exe"
                "${SF4E_HELPER_EXECUTABLE}"
            WORKING_DIRECTORY "${SF4E_HELPER_SOURCE}"
            DEPENDS ${SF4E_HELPER_RUST_SOURCES} "${SF4E_HELPER_SOURCE}/Cargo.toml"
                "${SF4E_HELPER_SOURCE}/Cargo.lock" "${SF4E_HELPER_SOURCE}/rust-toolchain.toml"
                "${SF4E_HELPER_SOURCE}/build.rs" "${SF4E_HELPER_SOURCE}/resource.rc" "${CMAKE_SOURCE_DIR}/src/ui/ember.ico"
            VERBATIM COMMENT "Build the pinned x64 Iroh helper")
        add_custom_target(IrohHelper ALL DEPENDS "${SF4E_HELPER_EXECUTABLE}")
        install(PROGRAMS "${SF4E_HELPER_EXECUTABLE}" DESTINATION ".")
        if(BUILD_TESTING)
            add_executable(HelperProcessTest "${CMAKE_SOURCE_DIR}/src/tests/helper_process_test.cxx")
            target_link_libraries(HelperProcessTest PRIVATE sf4e_helper_platform user32)
            add_dependencies(HelperProcessTest IrohHelper)
            add_test(NAME HelperProcess COMMAND HelperProcessTest "${SF4E_HELPER_EXECUTABLE}")
            set_tests_properties(HelperProcess PROPERTIES TIMEOUT 90)
            # The helper's own tests belong to the same gate as the C++ tests,
            # so a build receipt with testsPassed covers them (ledger A-015).
            # A separate target dir keeps the release helper from rebuilding.
            add_test(NAME HelperRust
                COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/rust-test-target"
                    "CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS=-C target-feature=+crt-static"
                    "${SF4E_CARGO_EXECUTABLE}" test --locked --target x86_64-pc-windows-msvc
                WORKING_DIRECTORY "${SF4E_HELPER_SOURCE}")
            set_tests_properties(HelperRust PROPERTIES TIMEOUT 1800)
        endif()
    endif()
endif()
