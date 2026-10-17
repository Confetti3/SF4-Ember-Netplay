# Services and the SDK do not depend on the Windows game helper.
option(SF4E_COMPONENT_TESTS "Register Rust service and SDK tests" ON)
if(BUILD_TESTING AND SF4E_COMPONENT_TESTS)
    include("${CMAKE_SOURCE_DIR}/cmake/sf4e_rust_toolchain.cmake")
    find_program(SF4E_CARGO_EXECUTABLE cargo REQUIRED)
    find_program(SF4E_PWSH_EXECUTABLE pwsh REQUIRED)
    find_program(SF4E_NODE_EXECUTABLE node REQUIRED)
    set(SF4E_CARGO_TEST_JOBS 2 CACHE STRING "Concurrent Cargo jobs per test suite")
    if(NOT SF4E_CARGO_TEST_JOBS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "SF4E_CARGO_TEST_JOBS must be a positive integer")
    endif()
    foreach(service IN ITEMS ember ember-short ember-rooms)
        if(service STREQUAL "ember")
            set(test_name EmberRust)
        else()
            set(test_name "${service}Rust")
        endif()
        add_test(NAME ${test_name}
            COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/rust-ember-target"
                "CARGO_INCREMENTAL=0" "RUSTUP_TOOLCHAIN=${SF4E_RUST_CHANNEL}"
                "${SF4E_CARGO_EXECUTABLE}" test --locked -j ${SF4E_CARGO_TEST_JOBS}
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/server/${service}")
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 1800 PROCESSORS ${SF4E_CARGO_TEST_JOBS})
        sf4e_test_locks(${test_name} ember_processes)
    endforeach()
    add_test(NAME BridgeSdk COMMAND "${SF4E_PWSH_EXECUTABLE}" -NoProfile -File
        "${CMAKE_SOURCE_DIR}/scripts/test-sdk.ps1" -BuildDir "${CMAKE_BINARY_DIR}" -Jobs ${SF4E_CARGO_TEST_JOBS}
        -Node "${SF4E_NODE_EXECUTABLE}" -Cargo "${SF4E_CARGO_EXECUTABLE}")
    set_tests_properties(BridgeSdk PROPERTIES TIMEOUT 1800 PROCESSORS ${SF4E_CARGO_TEST_JOBS})
    sf4e_test_locks(BridgeSdk ember_processes)
    add_test(NAME BuildValidation COMMAND "${SF4E_PWSH_EXECUTABLE}" -NoProfile -File
        "${CMAKE_SOURCE_DIR}/scripts/test-build-validation.ps1" -CMake "${CMAKE_COMMAND}")
    set_tests_properties(BuildValidation PROPERTIES TIMEOUT 90)
endif()
