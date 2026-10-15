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
#       [LOCKS <lock...>])
# A mistake stops the configure rather than dropping a setting: each option
# once, with values that are not empty, one each for NAME and the numbers. A
# mistyped option after SOURCES or LIBS lands in their list, so each source must
# exist and each library be a target or a name with a lowercase letter.
function(sf4e_add_unit_test)
    set(single NAME STD TIMEOUT SKIP_RETURN_CODE)
    set(keywords ${single} SOURCES LIBS LOCKS)
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
endfunction()
