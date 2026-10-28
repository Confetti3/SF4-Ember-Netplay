# The tests that run the Iroh helper, the room host and the room supervisor, registered after every
# library they link exists (CMakeLists.txt includes this file near its end). The ones that need the
# public Iroh relay and address lookup are registered only with SF4E_NETWORK_TESTS, carry the network
# label, and are not part of the default gate (scripts/BuildProvenance.ps1 excludes them).
# The registration helpers and the labels are in sf4e_tests.cmake.
if(NOT BUILD_TESTING OR NOT SF4E_BUILD_IROH_HELPER)
    return()
endif()
# Explicitly invoked comparison tool; network workloads are not CTest
# unit tests, and the benchmark executable is not installed in packages.
add_executable(RecoveryBenchmark "src/tests/recovery_benchmark.cxx")
target_compile_features(RecoveryBenchmark PRIVATE cxx_std_17)
target_link_libraries(RecoveryBenchmark PRIVATE sf4e_helper_platform ggpo::GGPO
    nlohmann_json::nlohmann_json ws2_32 psapi advapi32 winmm)
add_executable(RuntimeBootstrapTest "src/tests/runtime_bootstrap_test.cxx")
add_executable(PadAssignmentTest "src/tests/pad_assignment_test.cxx")
target_link_libraries(PadAssignmentTest PRIVATE Session sf4e Dimps sf4e_helper_platform "d3d9")
add_test(NAME PadAssignment COMMAND PadAssignmentTest)
target_link_libraries(RuntimeBootstrapTest PRIVATE Session sf4e sf4e_localization Dimps sf4e_helper_platform "d3d9")
add_dependencies(RuntimeBootstrapTest IrohHelper)
add_test(NAME RuntimeBootstrap COMMAND RuntimeBootstrapTest "${SF4E_HELPER_EXECUTABLE}")
sf4e_test_locks(RuntimeBootstrap loopback_udp_ports ember_processes ember_runtime)
set_tests_properties(RuntimeBootstrap PROPERTIES TIMEOUT 30)
sf4e_test_labels(LABELS unit TESTS PadAssignment)
sf4e_test_labels(LABELS helper TESTS RuntimeBootstrap)
add_executable(IrohRoomIntegrationTest "src/tests/iroh_room_integration_test.cxx")
target_link_libraries(IrohRoomIntegrationTest PRIVATE Session sf4e Dimps sf4e_helper_platform "d3d9")
add_dependencies(IrohRoomIntegrationTest IrohHelper)
add_executable(IrohRecoveryIntegrationTest "src/tests/iroh_recovery_integration_test.cxx")
target_link_libraries(IrohRecoveryIntegrationTest PRIVATE Session sf4e Dimps sf4e_helper_platform "d3d9")
add_dependencies(IrohRecoveryIntegrationTest IrohHelper)
add_executable(IrohGameIntegrationTest "src/tests/iroh_game_integration_test.cxx")
target_link_libraries(IrohGameIntegrationTest PRIVATE Session sf4e Dimps sf4e_helper_platform "d3d9")
add_dependencies(IrohGameIntegrationTest IrohHelper)
add_executable(IrohAuthorizedMatchTest "src/tests/iroh_authorized_match_test.cxx")
target_link_libraries(IrohAuthorizedMatchTest PRIVATE Session sf4e Dimps sf4e_helper_platform "d3d9")
add_dependencies(IrohAuthorizedMatchTest IrohHelper)
add_executable(CustomRoomGameTest "src/tests/custom_room_game_test.cxx")
target_link_libraries(CustomRoomGameTest PRIVATE Session sf4e Dimps sf4e_room_model ggpo::GGPO "d3d9")
add_dependencies(CustomRoomGameTest IrohHelper)
# End to end: the room host as a child process, real helpers, tickets
# minted by the ember-protocol example instead of a bridge.
add_executable(PublicRoomHostTest "src/tests/public_room_host_test.cxx")
target_link_libraries(PublicRoomHostTest PRIVATE Session sf4e Dimps sf4e_room_model "d3d9")
add_dependencies(PublicRoomHostTest IrohHelper sf4e-room-host)
# The soak harness for public rooms on a remote host (server/roomhost/soak).
# Not a test: it needs room hosts running elsewhere, so it is not
# registered with add_test.
add_executable(PublicRoomSoakTest "src/tests/public_room_soak_test.cxx" "src/tests/soak/soak_common.cxx"
    "src/tests/soak/soak_member.cxx" "src/tests/soak/soak_room.cxx" "src/tests/soak/soak_metrics.cxx"
    "src/tests/soak/soak_verdict.cxx" "src/tests/soak/soak_summary.cxx")
target_link_libraries(PublicRoomSoakTest PRIVATE Session sf4e Dimps sf4e_room_model "d3d9" psapi)
add_dependencies(PublicRoomSoakTest IrohHelper)
set(SF4E_ROOM_TICKET_TOOL "${CMAKE_BINARY_DIR}/rust-ember-target/debug/examples/room_ticket.exe")
add_custom_target(RoomTicketTool
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/rust-ember-target"
        "${SF4E_CARGO_EXECUTABLE}" build --locked --example room_ticket -p ember-protocol
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/server/ember"
    BYPRODUCTS "${SF4E_ROOM_TICKET_TOOL}"
    VERBATIM COMMENT "Build the room ticket test tool")
# The room supervisor, for the test that creates a public room through the
# bridge, the supervisor and a real room host.
set(SF4E_EMBER_ROOMS_TOOL "${CMAKE_BINARY_DIR}/rust-rooms-target/debug/ember-rooms.exe")
add_custom_target(EmberRoomsTool
    COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/rust-rooms-target"
        "${SF4E_CARGO_EXECUTABLE}" build --locked
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/server/ember-rooms"
    BYPRODUCTS "${SF4E_EMBER_ROOMS_TOOL}"
    VERBATIM COMMENT "Build the room supervisor")
option(SF4E_NETWORK_TESTS "Run tests requiring public Iroh relay/address lookup" OFF)
if(SF4E_NETWORK_TESTS)
    add_dependencies(PublicRoomHostTest RoomTicketTool)
    add_test(NAME PublicRoomHost COMMAND PublicRoomHostTest "${SF4E_HELPER_EXECUTABLE}"
        "$<TARGET_FILE:sf4e-room-host>" "${SF4E_ROOM_TICKET_TOOL}")
    set_tests_properties(PublicRoomHost PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
    add_test(NAME PublicRoomHostOwnDelay COMMAND PublicRoomHostTest "${SF4E_HELPER_EXECUTABLE}"
        "$<TARGET_FILE:sf4e-room-host>" "${SF4E_ROOM_TICKET_TOOL}" --own-delay)
    set_tests_properties(PublicRoomHostOwnDelay PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
    # A public room created through the helper, the bridge, the supervisor and a real
    # room host: the host's own report of each fighter's own delay reaches the bridge.
    add_dependencies(PublicRoomHostTest EmberRoomsTool)
    add_test(NAME PublicRoomsChain
        COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/rust-test-target"
            "CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS=-C target-feature=+crt-static"
            "EMBER_ROOMS_EXE=${SF4E_EMBER_ROOMS_TOOL}" "SF4E_ROOM_HOST_EXE=$<TARGET_FILE:sf4e-room-host>"
            "SF4E_HELPER_EXE=${SF4E_HELPER_EXECUTABLE}"
            "${SF4E_CARGO_EXECUTABLE}" test --locked --target x86_64-pc-windows-msvc
            --test public_rooms_bridge -- --ignored a_rooms_own_delay_reaches
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/rust/sf4-net")
    set_tests_properties(PublicRoomsChain PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
    add_test(NAME CustomRoomFourTables COMMAND CustomRoomGameTest "${SF4E_HELPER_EXECUTABLE}")
    add_test(NAME CustomRoomSpectators COMMAND CustomRoomGameTest "${SF4E_HELPER_EXECUTABLE}" --single-table)
    add_test(NAME CustomRoomFourTablesRelay COMMAND CustomRoomGameTest "${SF4E_HELPER_EXECUTABLE}" --relay-only)
    add_test(NAME CustomRoomSpectatorsRelay COMMAND CustomRoomGameTest "${SF4E_HELPER_EXECUTABLE}" --single-table --relay-only)
    # Six games after sixteen serialized admissions and table setup.
    # Each individual fixture phase retains its 45-second deadline.
    set_tests_properties(CustomRoomFourTables CustomRoomSpectators CustomRoomFourTablesRelay CustomRoomSpectatorsRelay PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE)
    # A spectator that never sets up must not hold the fighters' start.
    add_test(NAME CustomRoomStalledSpectator COMMAND CustomRoomGameTest "${SF4E_HELPER_EXECUTABLE}"
        --members 6 --single-table --cycles 2 --stall-spectator)
    set_tests_properties(CustomRoomStalledSpectator PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
    # A spectator retiring from the previous game must not hold the
    # rematch, and must finish its own teardown in bounded time.
    add_test(NAME CustomRoomLateSpectator COMMAND CustomRoomGameTest "${SF4E_HELPER_EXECUTABLE}"
        --members 3 --single-table --cycles 3 --late-spectator)
    set_tests_properties(CustomRoomLateSpectator PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
    # A spectator P1 drops before it synchronizes (still loading the
    # battle) must fail at once when P1 ends its link, not after 30 s.
    add_test(NAME CustomRoomSlowSpectator COMMAND CustomRoomGameTest "${SF4E_HELPER_EXECUTABLE}"
        --members 4 --single-table --cycles 2 --slow-spectator)
    set_tests_properties(CustomRoomSlowSpectator PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
    # Report-only timing runs: <name>;<members>;<members per table>.
    # The first two members at a table fight and the rest spectate.
    foreach(perf "SingleMatch;2;2" "TwoMatches;4;2" "FourMatches;8;2" "Spectators1;3;3"
            "Spectators4;6;6" "Spectators8;10;10" "SixteenFourTables;16;4")
        list(GET perf 0 perf_name)
        list(GET perf 1 perf_members)
        list(GET perf 2 perf_per_table)
        add_test(NAME RoomPerf.${perf_name} COMMAND CustomRoomGameTest "${SF4E_HELPER_EXECUTABLE}" --perf
            --members ${perf_members} --per-table ${perf_per_table} --cycles 2
            --json "${CMAKE_BINARY_DIR}/room-perf-${perf_name}.json")
        set_tests_properties(RoomPerf.${perf_name} PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE)
        sf4e_test_labels(LABELS network room TESTS RoomPerf.${perf_name})
    endforeach()
    add_test(NAME IrohRoomIntegration COMMAND IrohRoomIntegrationTest "${SF4E_HELPER_EXECUTABLE}")
    add_test(NAME IrohRoomRelayIntegration COMMAND IrohRoomIntegrationTest "${SF4E_HELPER_EXECUTABLE}" --relay-only)
    set_tests_properties(IrohRoomIntegration IrohRoomRelayIntegration PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
    add_test(NAME IrohRoomQueueAcks COMMAND IrohRoomIntegrationTest "${SF4E_HELPER_EXECUTABLE}" --queue-acks)
    add_test(NAME IrohRoomQueueAcksRelay COMMAND IrohRoomIntegrationTest "${SF4E_HELPER_EXECUTABLE}" --queue-acks --relay-only)
    set_tests_properties(IrohRoomQueueAcks IrohRoomQueueAcksRelay PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    add_test(NAME IrohRecoveryIntegration COMMAND IrohRecoveryIntegrationTest "${SF4E_HELPER_EXECUTABLE}")
    add_test(NAME IrohRecoveryRelayIntegration COMMAND IrohRecoveryIntegrationTest "${SF4E_HELPER_EXECUTABLE}" --relay-only)
    # Eight independent recovery windows, including real GGPO traffic.
    # Each fixture phase retains its own bounded deadline.
    set_tests_properties(IrohRecoveryIntegration IrohRecoveryRelayIntegration PROPERTIES TIMEOUT 420 RUN_SERIAL TRUE)
    add_test(NAME IrohGameIntegration COMMAND IrohGameIntegrationTest "${SF4E_HELPER_EXECUTABLE}")
    add_test(NAME IrohGameRelayIntegration COMMAND IrohGameIntegrationTest "${SF4E_HELPER_EXECUTABLE}" --relay-only)
    set_tests_properties(IrohGameIntegration IrohGameRelayIntegration PROPERTIES TIMEOUT 90)
    add_test(NAME IrohAuthorizedMatch COMMAND IrohAuthorizedMatchTest "${SF4E_HELPER_EXECUTABLE}")
    add_test(NAME IrohAuthorizedRelayMatch COMMAND IrohAuthorizedMatchTest "${SF4E_HELPER_EXECUTABLE}" --relay-only)
    add_test(NAME IrohAuthorizedTerminalRecovery COMMAND IrohAuthorizedMatchTest "${SF4E_HELPER_EXECUTABLE}" --terminal-recovery)
    add_test(NAME IrohAuthorizedTerminalRelayRecovery COMMAND IrohAuthorizedMatchTest "${SF4E_HELPER_EXECUTABLE}" --terminal-recovery --relay-only)
    set_tests_properties(IrohAuthorizedMatch IrohAuthorizedRelayMatch PROPERTIES TIMEOUT 120)
    set_tests_properties(IrohAuthorizedTerminalRecovery IrohAuthorizedTerminalRelayRecovery PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
    sf4e_test_labels(LABELS network room TESTS PublicRoomHost PublicRoomHostOwnDelay CustomRoomFourTables CustomRoomSpectators
        CustomRoomFourTablesRelay CustomRoomSpectatorsRelay CustomRoomStalledSpectator CustomRoomLateSpectator CustomRoomSlowSpectator)
    sf4e_test_labels(LABELS network rust TESTS PublicRoomsChain)
    sf4e_test_labels(LABELS network helper TESTS IrohRoomIntegration IrohRoomRelayIntegration IrohRoomQueueAcks IrohRoomQueueAcksRelay
        IrohRecoveryIntegration IrohRecoveryRelayIntegration IrohGameIntegration IrohGameRelayIntegration IrohAuthorizedMatch
        IrohAuthorizedRelayMatch IrohAuthorizedTerminalRecovery IrohAuthorizedTerminalRelayRecovery)
endif()
