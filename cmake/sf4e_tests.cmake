# CTest runs the tests side by side. A test that uses something another test
# also uses names its lock where it is registered, and no two tests holding
# the same lock run together:
#   loopback_udp_ports  binds a loopback UDP port, directly, through GGPO or
#       through the helper. Several close a socket and hand its port to GGPO,
#       which binds it a moment later; a process binding port 0 in between can
#       be given the same port.
#   ember_processes     starts sf4-net, or (UpgradeRecoveryPowerShell) refuses
#       while it lists an sf4-net whose path it cannot read, as for a helper
#       that exits during its check.
#   ember_runtime       starts the netplay runtime or reads the overlay's
#       settings: both open the player's settings.lock without sharing, and the
#       runtime opens the fixed Local\SF4Ember* link mailboxes.
# Any other test's files, pipes and named objects carry a pid or random name.
set(SF4E_TEST_LOCKS loopback_udp_ports ember_processes ember_runtime)

# Every test carries at least one label, so a retest can pick a family with
# `ctest -L <label>` (or leave one out with -LE) instead of a name pattern. A
# label outside this list, or a test without any, stops the configure.
#   unit       one native test executable (every sf4e_add_unit_test test has it)
#   ui         the overlay, menus and shell: it links sf4e_ui or sf4e_recovery,
#              or renders the screens
#   room       the room model, the session server, the room host, the Iroh room
#              and the tournament and watch play: it links their libraries or is
#              named for them
#   ggpo       GGPO over loopback UDP
#   replay     replay files, the archive, the recorder and playback
#   training   training mode
#   discord    the Discord companion
#   helper     runs the Rust helper (sf4-net.exe) or tests the code that starts it
#   rust       a cargo test suite (HelperRust, EmberRust, PublicRoomsChain)
#   sdk        the TypeScript SDK against the bridge (BridgeSdk)
#   publisher  a PowerShell suite for building, packaging and publishing
#              (BuildProvenance, NightlyPublication, NightlyPaths, PackageFiles)
#   script     any PowerShell suite, the publisher's and the updater's
#   network    needs the public Iroh relay and address lookup (SF4E_NETWORK_TESTS)
# The area labels are given where a test links the area's library or is named
# for it, so they select the main tests of an area rather than every test that
# touches it. HelperRust and PublicRoomsChain build into rust-test-target in the
# build folder, EmberRust and BridgeSdk into rust-ember-target: a targeted cargo
# run must not write into either folder while ctest runs a suite that uses it.
set(SF4E_TEST_LABELS unit ui room ggpo replay training discord helper rust sdk publisher script network)
#   sf4e_test_labels(LABELS <label...> TESTS <test...>)
# Adds the labels to tests that exist already, whatever registered them.
function(sf4e_test_labels)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "" "LABELS;TESTS")
    if(arg_UNPARSED_ARGUMENTS OR NOT arg_LABELS OR NOT arg_TESTS)
        message(FATAL_ERROR "sf4e_test_labels: needs LABELS <label...> TESTS <test...>")
    endif()
    foreach(label IN LISTS arg_LABELS)
        if(NOT label IN_LIST SF4E_TEST_LABELS)
            message(FATAL_ERROR "sf4e_test_labels: '${label}' is not one of ${SF4E_TEST_LABELS}")
        endif()
    endforeach()
    foreach(test IN LISTS arg_TESTS)
        if(NOT TEST "${test}")
            message(FATAL_ERROR "sf4e_test_labels: no test named '${test}'")
        endif()
        set_property(TEST "${test}" APPEND PROPERTY LABELS ${arg_LABELS})
    endforeach()
endfunction()
# Stops the configure when a registered test has no label. Called where
# registration ends, so a test added with a bare add_test is noticed.
function(sf4e_check_test_labels)
    get_property(tests DIRECTORY PROPERTY TESTS)
    set(unlabeled "")
    foreach(test IN LISTS tests)
        get_property(labels TEST "${test}" PROPERTY LABELS)
        if(NOT labels)
            list(APPEND unlabeled "${test}")
        endif()
    endforeach()
    if(unlabeled)
        message(FATAL_ERROR "Tests without a label (sf4e_test_labels in cmake/sf4e_tests.cmake): ${unlabeled}")
    endif()
endfunction()
#   sf4e_test_locks(<test> <lock...>)
function(sf4e_test_locks test)
    if(ARGC LESS 2)
        message(FATAL_ERROR "sf4e_test_locks(${test}): needs a lock")
    endif()
    foreach(lock IN LISTS ARGN)
        if(NOT lock IN_LIST SF4E_TEST_LOCKS)
            message(FATAL_ERROR "sf4e_test_locks(${test}): '${lock}' is not one of ${SF4E_TEST_LOCKS}")
        endif()
    endforeach()
    set_tests_properties(${test} PROPERTIES RESOURCE_LOCK "${ARGN}")
endfunction()

# A unit test that is one executable, NAME followed by Test, run once with no
# arguments as CTest's NAME; anything else is registered by hand. Without STD
# the target takes the directory's standard.
#   sf4e_add_unit_test(NAME <Name> SOURCES <files...> [LIBS <libs...>]
#       [STD <11|14|17>] [TIMEOUT <seconds>] [SKIP_RETURN_CODE <code>]
#       [LOCKS <lock...>] [LABELS <label...>])
# The test is labeled unit, and with LABELS as well.
# A mistake stops the configure rather than dropping a setting: each option
# once, with values that are not empty, one each for NAME and the numbers. A
# mistyped option after SOURCES or LIBS lands in their list, so each source must
# exist and each library be a target or a name with a lowercase letter.
function(sf4e_add_unit_test)
    set(single NAME STD TIMEOUT SKIP_RETURN_CODE)
    set(keywords ${single} SOURCES LIBS LOCKS LABELS)
    foreach(keyword IN LISTS keywords)
        set(arg_${keyword} "")
    endforeach()
    set(seen "")
    set(current "")
    if(ARGC EQUAL 0)
        message(FATAL_ERROR "sf4e_add_unit_test: needs NAME and SOURCES")
    endif()
    # ARGV<n> one by one, since ARGN loses empty arguments.
    math(EXPR last "${ARGC} - 1")
    foreach(i RANGE 0 ${last})
        set(token "${ARGV${i}}")
        if("${token}" IN_LIST keywords)
            if("${token}" IN_LIST seen)
                message(FATAL_ERROR "sf4e_add_unit_test: ${token} is given twice")
            endif()
            list(APPEND seen "${token}")
            set(current "${token}")
        elseif("${current}" STREQUAL "")
            message(FATAL_ERROR "sf4e_add_unit_test: unexpected '${token}' before any option")
        elseif("${token}" STREQUAL "")
            message(FATAL_ERROR "sf4e_add_unit_test: ${current} has an empty value")
        elseif("${current}" IN_LIST single AND NOT "${arg_${current}}" STREQUAL "")
            message(FATAL_ERROR "sf4e_add_unit_test: ${current} takes one value; unexpected '${token}'")
        else()
            list(APPEND arg_${current} "${token}")
        endif()
    endforeach()
    foreach(keyword IN LISTS seen)
        if("${arg_${keyword}}" STREQUAL "")
            message(FATAL_ERROR "sf4e_add_unit_test: ${keyword} has no value")
        endif()
    endforeach()
    if(NOT "${arg_NAME}" MATCHES "^[A-Za-z][A-Za-z0-9_]*$" OR NOT "SOURCES" IN_LIST seen)
        message(FATAL_ERROR "sf4e_add_unit_test(${arg_NAME}): needs NAME and SOURCES")
    endif()
    foreach(source IN LISTS arg_SOURCES)
        if(NOT IS_ABSOLUTE "${source}")
            set(source "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        endif()
        if(NOT EXISTS "${source}" OR IS_DIRECTORY "${source}")
            message(FATAL_ERROR "sf4e_add_unit_test(${arg_NAME}): no source file '${source}'")
        endif()
    endforeach()
    foreach(lib IN LISTS arg_LIBS)
        # A platform condition around one library, as in $<$<PLATFORM_ID:Windows>:bcrypt>.
        string(REGEX REPLACE "^\\$<\\$<PLATFORM_ID:[A-Za-z]+>:(.+)>$" "\\1" name "${lib}")
        if(NOT TARGET "${name}" AND NOT (name MATCHES "^[A-Za-z][A-Za-z0-9_]*$" AND name MATCHES "[a-z]"))
            message(FATAL_ERROR "sf4e_add_unit_test(${arg_NAME}): '${lib}' is not a target or a library name")
        endif()
    endforeach()
    foreach(number STD TIMEOUT SKIP_RETURN_CODE)
        if("${number}" IN_LIST seen AND NOT "${arg_${number}}" MATCHES "^[0-9]+$")
            message(FATAL_ERROR "sf4e_add_unit_test(${arg_NAME}): ${number} '${arg_${number}}' is not a number")
        endif()
    endforeach()
    set(target "${arg_NAME}Test")
    add_executable(${target} ${arg_SOURCES})
    if("STD" IN_LIST seen)
        target_compile_features(${target} PRIVATE cxx_std_${arg_STD})
    endif()
    if("LIBS" IN_LIST seen)
        target_link_libraries(${target} PRIVATE ${arg_LIBS})
    endif()
    add_test(NAME ${arg_NAME} COMMAND ${target})
    foreach(property TIMEOUT SKIP_RETURN_CODE)
        if("${property}" IN_LIST seen)
            set_tests_properties(${arg_NAME} PROPERTIES ${property} ${arg_${property}})
        endif()
    endforeach()
    if("LOCKS" IN_LIST seen)
        sf4e_test_locks(${arg_NAME} ${arg_LOCKS})
    endif()
    sf4e_test_labels(LABELS unit ${arg_LABELS} TESTS ${arg_NAME})
endfunction()
