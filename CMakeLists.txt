cmake_minimum_required (VERSION 3.20)

include("${CMAKE_SOURCE_DIR}/cmake/sf4e_version.cmake")

project (
    sf4e
    VERSION 0.8.1
    DESCRIPTION "SF4 Ember Netplay: private rollback rooms and an in-game overlay for Ultra Street Fighter IV."
    HOMEPAGE_URL "https://github.com/Confetti3/SF4-Ember-Netplay"
)

include(CTest)
include("${CMAKE_SOURCE_DIR}/cmake/sf4e_helper.cmake")

option(SF4E_CORE_TESTS_ONLY "Build game-independent migration tests without Windows/game dependencies" OFF)
add_library(sf4e_netplay_core STATIC "src/netplay/SessionController.cxx" "src/netplay/MatchResultOutbox.cxx")
add_library(sf4e_selection STATIC "src/common/FighterCatalog.cxx" "src/common/StageCatalog.cxx")
target_compile_features(sf4e_selection PUBLIC cxx_std_11)
target_compile_features(sf4e_netplay_core PUBLIC cxx_std_11)
if(BUILD_TESTING)
    add_executable(RoomMessageQueueTest "src/tests/room_message_queue_test.cxx")
    target_link_libraries(RoomMessageQueueTest PRIVATE sf4e_room_model)
    add_test(NAME RoomMessageQueue COMMAND RoomMessageQueueTest)
    add_executable(NativeRepairTimingTest "src/tests/native_repair_timing_test.cxx")
    target_compile_features(NativeRepairTimingTest PRIVATE cxx_std_17)
    add_test(NAME NativeRepairTiming COMMAND NativeRepairTimingTest)
    find_package(Threads REQUIRED)
    add_executable(TrainingSessionTest "src/tests/training_session_test.cxx")
    target_compile_features(TrainingSessionTest PRIVATE cxx_std_14)
    add_test(NAME TrainingSession COMMAND TrainingSessionTest)
    add_executable(FighterCatalogTest "src/tests/fighter_catalog_test.cxx")
    target_link_libraries(FighterCatalogTest PRIVATE sf4e_selection)
    add_test(NAME FighterCatalog COMMAND FighterCatalogTest)
    set_tests_properties(FighterCatalog PROPERTIES TIMEOUT 15)
    add_executable(SessionControllerTest "src/tests/session_controller_test.cxx")
    target_link_libraries(SessionControllerTest PRIVATE sf4e_netplay_core Threads::Threads)
    add_test(NAME SessionController COMMAND SessionControllerTest)
    set_tests_properties(SessionController PROPERTIES TIMEOUT 15)
    add_executable(RollbackHudTest "src/tests/rollback_hud_test.cxx")
    add_test(NAME RollbackHud COMMAND RollbackHudTest)
    add_executable(NativeMatchResultTest "src/tests/native_match_result_test.cxx")
    target_compile_features(NativeMatchResultTest PRIVATE cxx_std_11)
    add_test(NAME NativeMatchResult COMMAND NativeMatchResultTest)
    set_tests_properties(NativeMatchResult PROPERTIES TIMEOUT 15)
endif()
if(SF4E_CORE_TESTS_ONLY)
    return()
endif()

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG")
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)
add_compile_definitions(WIN32_LEAN_AND_MEAN)

find_library(LIB_DETOURS "detours")
find_package(CLI11 CONFIG REQUIRED)
find_package(ggpo CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
if(BUILD_TESTING)
    add_executable(CheckpointTransferTest "src/tests/checkpoint_transfer_test.cxx")
    target_compile_features(CheckpointTransferTest PRIVATE cxx_std_17)
    target_link_libraries(CheckpointTransferTest PRIVATE nlohmann_json::nlohmann_json bcrypt)
    add_test(NAME CheckpointTransfer COMMAND CheckpointTransferTest)
endif()
include("${CMAKE_SOURCE_DIR}/cmake/sf4e_discord.cmake")
find_package(spdlog CONFIG REQUIRED)
find_package(wil CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

add_library(sf4e_room_model STATIC "src/session/RoomModel.cxx")
target_compile_features(sf4e_room_model PUBLIC cxx_std_14)
target_link_libraries(sf4e_room_model PUBLIC nlohmann_json::nlohmann_json)
if(BUILD_TESTING)
    add_executable(MatchResultOutboxTest "src/tests/match_result_outbox_test.cxx")
    target_link_libraries(MatchResultOutboxTest PRIVATE sf4e_netplay_core sf4e_room_model)
    add_test(NAME MatchResultOutbox COMMAND MatchResultOutboxTest)
    set_tests_properties(MatchResultOutbox PROPERTIES TIMEOUT 15)
endif()
if(BUILD_TESTING)
    add_executable(DiscordPresenceTest src/tests/discord_presence_test.cxx)
    target_link_libraries(DiscordPresenceTest PRIVATE sf4e_room_model)
    add_test(NAME DiscordPresence COMMAND DiscordPresenceTest)
    include(ExternalProject)
    string(REGEX REPLACE "/VC/.*$" "" discord_vs_instance "${CMAKE_CXX_COMPILER}")
    ExternalProject_Add(DiscordBridgeFixture
        BUILD_ALWAYS TRUE
        SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/tests/discord-fixture"
        BINARY_DIR "${CMAKE_BINARY_DIR}/discord-bridge-fixture"
        CMAKE_GENERATOR "Visual Studio 18 2026"
        CMAKE_GENERATOR_PLATFORM x64
        CMAKE_GENERATOR_INSTANCE "${discord_vs_instance}"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release
        INSTALL_COMMAND "")
    add_executable(DiscordBridgeTest src/tests/discord_bridge_test.cxx)
    target_link_libraries(DiscordBridgeTest PRIVATE sf4e_helper_platform advapi32)
    add_dependencies(DiscordBridgeTest DiscordBridgeFixture)
    add_test(NAME DiscordBridge COMMAND DiscordBridgeTest "${CMAKE_BINARY_DIR}/discord-bridge-fixture/Release/DiscordBridgeFixture.exe")
    set_tests_properties(DiscordBridge PROPERTIES TIMEOUT 30)
    # Manual live-Discord check; never part of unattended CTest runs.
    add_executable(DiscordSmokeTest src/tests/discord_smoke_test.cxx)
    target_link_libraries(DiscordSmokeTest PRIVATE sf4e_helper_platform nlohmann_json::nlohmann_json)
endif()
if(BUILD_TESTING)
    add_executable(RoomAuthorityTest "src/tests/room_authority_test.cxx")
    target_link_libraries(RoomAuthorityTest PRIVATE sf4e_room_model)
    add_test(NAME RoomAuthority COMMAND RoomAuthorityTest)
    set_tests_properties(RoomAuthority PROPERTIES TIMEOUT 30)
    add_executable(SessionRecoveryGateTest "src/tests/session_recovery_gate_test.cxx")
    target_compile_features(SessionRecoveryGateTest PRIVATE cxx_std_14)
    target_link_libraries(SessionRecoveryGateTest PRIVATE sf4e_room_model)
    if(MSVC)
        target_compile_options(SessionRecoveryGateTest PRIVATE /UNDEBUG)
    else()
        target_compile_options(SessionRecoveryGateTest PRIVATE -UNDEBUG)
    endif()
    add_test(NAME SessionRecoveryGate COMMAND SessionRecoveryGateTest)
endif()

if(BUILD_TESTING)
    add_executable(SelectionValueTest "src/tests/selection_value_test.cxx")
    target_link_libraries(SelectionValueTest PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME SelectionValue COMMAND SelectionValueTest)
    set_tests_properties(SelectionValue PROPERTIES TIMEOUT 15)
endif()

add_library(sf4e_settings STATIC "src/netplay/SettingsStore.cxx" "src/netplay/SettingsWriter.cxx")
target_compile_features(sf4e_settings PUBLIC cxx_std_17)
target_link_libraries(sf4e_settings PUBLIC nlohmann_json::nlohmann_json PRIVATE "shell32" "ole32")
if(BUILD_TESTING)
    add_executable(SettingsStoreTest "src/tests/settings_store_test.cxx")
    target_link_libraries(SettingsStoreTest PRIVATE sf4e_settings)
    add_test(NAME SettingsStore COMMAND SettingsStoreTest)
    set_tests_properties(SettingsStore PROPERTIES TIMEOUT 15)
endif()

add_library(sf4e_ui STATIC "src/ui/ApplicationShell.cxx" "src/ui/RoomPanel.cxx" "src/ui/Theme.cxx" "src/ui/SelectionArt.cxx" "src/ui/FighterSelector.cxx")
target_sources(sf4e_ui PRIVATE src/ui/Win32Input.cxx)
target_sources(sf4e_ui PRIVATE src/ui/GameMenu.cxx)
install(DIRECTORY "assets/brand" DESTINATION "assets" FILES_MATCHING PATTERN "*.png")
install(DIRECTORY "assets/input" DESTINATION "assets" FILES_MATCHING PATTERN "*.png" PATTERN "*.txt")
target_sources(sf4e_ui PRIVATE src/ui/ControllerNavigation.cxx src/ui/backends/imgui_impl_win32.cpp)
set_source_files_properties(src/ui/backends/imgui_impl_win32.cpp PROPERTIES COMPILE_DEFINITIONS IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
target_sources(sf4e_ui PRIVATE src/ui/TrainingPanel.cxx)
target_compile_features(sf4e_ui PUBLIC cxx_std_14)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sf4e_fonts.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sf4e_brand.cmake")
target_link_libraries(sf4e_ui PUBLIC imgui::imgui sf4e_selection sf4e_room_model PRIVATE ZLIB::ZLIB "windowscodecs" "ole32" "d3d9")
target_compile_definitions(sf4e_ui PRIVATE SF4E_BUILD_VERSION="${PROJECT_VERSION}")
if(BUILD_TESTING)
    add_executable(UiRenderTest "src/tests/ui_render_test.cxx")
    target_compile_definitions(UiRenderTest PRIVATE SF4E_TEST_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets/selection")
    add_executable(ControllerNavigationTest src/tests/controller_navigation_test.cxx)
    target_link_libraries(ControllerNavigationTest PRIVATE sf4e_ui Dimps)
    add_test(NAME ControllerNavigation COMMAND ControllerNavigationTest)
    set_tests_properties(ControllerNavigation PROPERTIES TIMEOUT 30)
    add_executable(RoomPanelNavigationTest src/tests/room_panel_navigation_test.cxx)
    target_link_libraries(RoomPanelNavigationTest PRIVATE sf4e_ui Dimps)
    add_test(NAME RoomPanelNavigation COMMAND RoomPanelNavigationTest)
    set_tests_properties(RoomPanelNavigation PROPERTIES TIMEOUT 30)
    add_executable(NativeSelectionAvailabilityTest src/tests/native_selection_availability_test.cxx)
    target_link_libraries(NativeSelectionAvailabilityTest PRIVATE sf4e_ui Dimps)
    add_test(NAME NativeSelectionAvailability COMMAND NativeSelectionAvailabilityTest)
    set_tests_properties(NativeSelectionAvailability PROPERTIES TIMEOUT 30)
    add_executable(Win32CursorTest src/tests/win32_cursor_test.cxx)
    target_link_libraries(Win32CursorTest PRIVATE sf4e_ui)
    add_test(NAME Win32Cursor COMMAND Win32CursorTest)
    target_link_libraries(UiRenderTest PRIVATE sf4e_ui "d3d9")
    add_test(NAME UiRender COMMAND UiRenderTest)
    set_tests_properties(UiRender PROPERTIES TIMEOUT 90)
endif()

find_path(VALVEFILEVDF_INCLUDE_DIRS "vdf_parser.hpp")

add_library(Dimps STATIC
    "src/Dimps/Dimps__Selection.cxx"
    "src/Dimps/Dimps.hxx"
    "src/Dimps/Dimps.cxx"
    "src/Dimps/Dimps__Eva.hxx"
    "src/Dimps/Dimps__Eva.cxx"
    "src/Dimps/Dimps__Event.hxx"
    "src/Dimps/Dimps__Event.cxx"
    "src/Dimps/Dimps__Game.hxx"
    "src/Dimps/Dimps__Game.cxx"
    "src/Dimps/Dimps__Game__Battle.hxx"
    "src/Dimps/Dimps__Game__Battle.cxx"
    "src/Dimps/Dimps__Game__Battle__Action.hxx"
    "src/Dimps/Dimps__Game__Battle__Action.cxx"
    "src/Dimps/Dimps__Game__Battle__Camera.hxx"
    "src/Dimps/Dimps__Game__Battle__Camera.cxx"
    "src/Dimps/Dimps__Game__Battle__Chara.hxx"
    "src/Dimps/Dimps__Game__Battle__Chara.cxx"
    "src/Dimps/Dimps__Game__Battle__Command.hxx"
    "src/Dimps/Dimps__Game__Battle__Command.cxx"
    "src/Dimps/Dimps__Game__Battle__Effect.hxx"
    "src/Dimps/Dimps__Game__Battle__Effect.cxx"
    "src/Dimps/Dimps__Game__Battle__Hud.hxx"
    "src/Dimps/Dimps__Game__Battle__Hud.cxx"
    "src/Dimps/Dimps__Game__Battle__System.hxx"
    "src/Dimps/Dimps__Game__Battle__System.cxx"
    "src/Dimps/Dimps__Game__Battle__Training.hxx"
    "src/Dimps/Dimps__Game__Battle__Training.cxx"
    "src/Dimps/Dimps__Game__Battle__Vfx.hxx"
    "src/Dimps/Dimps__Game__Battle__Vfx.cxx"
    "src/Dimps/Dimps__GameEvents.hxx"
    "src/Dimps/Dimps__GameEvents.cxx"
    "src/Dimps/Dimps__Math.hxx"
    "src/Dimps/Dimps__Math.cxx"
    "src/Dimps/Dimps__Pad.hxx"
    "src/Dimps/Dimps__Pad.cxx"
    "src/Dimps/Dimps__Platform.hxx"
    "src/Dimps/Dimps__Platform.cxx"
    "src/Dimps/Dimps__UserApp.hxx"
    "src/Dimps/Dimps__UserApp.cxx"
)
target_link_libraries(
    Dimps
    PUBLIC nlohmann_json::nlohmann_json sf4e_selection
)

add_library(sf4e_common STATIC
    "src/common/sf4e__NetUtil.cxx"
    "src/common/sf4e__NetUtil.hxx"
    "src/common/sf4e__RollbackDiagnostics.cxx"
    "src/common/sf4e__RollbackDiagnostics.hxx"
    "src/common/sf4e__GgpoGate.cxx"
    "src/common/sf4e__GgpoGate.hxx"
    "src/common/sf4e__PacingController.hxx"
    "src/common/install_paths.cxx"
    "src/common/install_paths.hxx"
)
target_link_libraries(sf4e_common PUBLIC "iphlpapi" "ws2_32" "winhttp")

add_library(sf4e STATIC
    "src/sf4e/sf4e__InputDevices.cxx"
    "src/sf4e/sf4e.hxx"
    "src/sf4e/sf4e.cxx"
    "src/sf4e/sf4e__Event.hxx"
    "src/sf4e/sf4e__Event.cxx"
    "src/sf4e/sf4e__Game.hxx"
    "src/sf4e/sf4e__Game.cxx"
    "src/sf4e/sf4e__Game__Battle.hxx"
    "src/sf4e/sf4e__Game__Battle.cxx"
    "src/sf4e/sf4e__Game__Battle__Effect.hxx"
    "src/sf4e/sf4e__Game__Battle__Effect.cxx"
    "src/sf4e/sf4e__Game__Battle__Hud.hxx"
    "src/sf4e/sf4e__Game__Battle__Hud.cxx"
    "src/sf4e/sf4e__Game__Battle__System.hxx"
    "src/sf4e/sf4e__Game__Battle__System.cxx"
    "src/sf4e/sf4e__Game__Battle__Vfx.hxx"
    "src/sf4e/sf4e__Game__Battle__Vfx.cxx"
    "src/sf4e/sf4e__GameEvents.hxx"
    "src/sf4e/sf4e__GameEvents.cxx"
    "src/sf4e/sf4e__Overlay.hxx"
    "src/sf4e/sf4e__Overlay.cxx"
    "src/training/TrainingRuntime.cxx"
    "src/sf4e/sf4e__OverlayPrefs.hxx"
    "src/sf4e/sf4e__OverlayPrefs.cxx"
    "src/sf4e/sf4e__Pad.hxx"
    "src/sf4e/sf4e__Pad.cxx"
    "src/sf4e/sf4e__Platform.hxx"
    "src/sf4e/sf4e__Platform.cxx"
    "src/sf4e/sf4e__UserApp.hxx"
    "src/sf4e/sf4e__UserApp.cxx"
    "src/sf4e/sf4e__NetplayFacade.hxx"
    "src/sf4e/sf4e__NetplayFacade.cxx"
    "src/sf4e/sf4e__Application.cxx"
)
target_include_directories(sf4e PUBLIC "src/common")
option(SF4E_DEVELOPER_UI "Build the fixed developer inspector page" OFF)
if(SF4E_DEVELOPER_UI)
    target_sources(sf4e PRIVATE "src/sf4e/sf4e__DeveloperOverlay.cxx")
    target_compile_definitions(sf4e PRIVATE SF4E_DEVELOPER_UI=1)
endif()
target_link_libraries(sf4e PRIVATE sf4e_netplay_core sf4e_helper_platform sf4e_settings sf4e_ui)
target_link_libraries(
    sf4e
    PRIVATE ${LIB_DETOURS} Session sf4e_common "Kernel32" "ws2_32" "Mswsock" "AdvApi32" "Shlwapi" "pathcch" "shell32" "Bcrypt" "Xinput9_1_0" "iphlpapi"
    PUBLIC imgui::imgui nlohmann_json::nlohmann_json spdlog::spdlog  ggpo::GGPO
)

add_library(Session STATIC
    "src/session/sf4e__SessionClient.hxx"
    "src/session/sf4e__SessionClient.cxx"
    "src/session/sf4e__SessionProtocol.hxx"
    "src/session/sf4e__SessionProtocol.cxx"
    "src/session/sf4e__SessionServer.hxx"
    "src/session/sf4e__SessionServer.cxx"
    "src/session/IrohRoom.cxx"
    "src/session/MatchAuthority.cxx"
    "src/session/IrohMatchSession.cxx"
    "src/session/IrohRoom.hxx"
    "src/session/SessionTransport.hxx"
)
target_compile_features(Session PUBLIC cxx_std_17)
target_link_libraries(
    Session
    PRIVATE Dimps sf4e_common sf4e_helper_platform sf4e_room_model
    PUBLIC nlohmann_json::nlohmann_json spdlog::spdlog  "ws2_32"
)

if(BUILD_TESTING)
    add_executable(SessionClientTransportTest "src/tests/session_client_transport_test.cxx")
    target_compile_features(SessionClientTransportTest PRIVATE cxx_std_17)
    target_link_libraries(SessionClientTransportTest PRIVATE Session sf4e Dimps "d3d9")
    add_test(NAME SessionClientTransport COMMAND SessionClientTransportTest)
    if(SF4E_BUILD_IROH_HELPER)
        # Explicitly invoked comparison tool; network workloads are not CTest
        # unit tests, and the benchmark executable is not installed in packages.
        add_executable(RecoveryBenchmark "src/tests/recovery_benchmark.cxx")
        target_compile_features(RecoveryBenchmark PRIVATE cxx_std_17)
        target_link_libraries(RecoveryBenchmark PRIVATE sf4e_helper_platform ggpo::GGPO
            nlohmann_json::nlohmann_json ws2_32 psapi advapi32)
        add_executable(RuntimeBootstrapTest "src/tests/runtime_bootstrap_test.cxx")
        add_executable(PadAssignmentTest "src/tests/pad_assignment_test.cxx")
        target_link_libraries(PadAssignmentTest PRIVATE Session sf4e Dimps sf4e_helper_platform "d3d9")
        add_test(NAME PadAssignment COMMAND PadAssignmentTest)
        target_link_libraries(RuntimeBootstrapTest PRIVATE Session sf4e Dimps sf4e_helper_platform "d3d9")
        add_dependencies(RuntimeBootstrapTest IrohHelper)
        add_test(NAME RuntimeBootstrap COMMAND RuntimeBootstrapTest "${SF4E_HELPER_EXECUTABLE}")
        set_tests_properties(RuntimeBootstrap PROPERTIES TIMEOUT 30)
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
        option(SF4E_NETWORK_TESTS "Run tests requiring public Iroh relay/address lookup" OFF)
        if(SF4E_NETWORK_TESTS)
            add_test(NAME CustomRoomFourTables COMMAND CustomRoomGameTest "${SF4E_HELPER_EXECUTABLE}")
            add_test(NAME CustomRoomSpectators COMMAND CustomRoomGameTest "${SF4E_HELPER_EXECUTABLE}" --single-table)
            add_test(NAME CustomRoomFourTablesRelay COMMAND CustomRoomGameTest "${SF4E_HELPER_EXECUTABLE}" --relay-only)
            add_test(NAME CustomRoomSpectatorsRelay COMMAND CustomRoomGameTest "${SF4E_HELPER_EXECUTABLE}" --single-table --relay-only)
            # Six games after sixteen serialized admissions and table setup.
            # Each individual fixture phase retains its 45-second deadline.
            set_tests_properties(CustomRoomFourTables CustomRoomSpectators CustomRoomFourTablesRelay CustomRoomSpectatorsRelay PROPERTIES TIMEOUT 1800 RUN_SERIAL TRUE)
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
        endif()
    endif()
    add_executable(SessionServerTransportTest
        "src/tests/session_server_transport_test.cxx"
        "src/session/MatchAuthority.cxx"
        "src/session/sf4e__SessionServer.cxx"
        "src/session/sf4e__SessionProtocol.cxx")
    target_link_libraries(SessionServerTransportTest PRIVATE
        nlohmann_json::nlohmann_json spdlog::spdlog sf4e_selection sf4e_room_model "bcrypt")
    target_compile_features(SessionServerTransportTest PRIVATE cxx_std_14)
    add_test(NAME SessionServerTransport COMMAND SessionServerTransportTest)
    add_executable(GgpoUdpValidationTest "src/tests/ggpo_udp_validation_test.cxx")
    target_link_libraries(GgpoUdpValidationTest PRIVATE ggpo::GGPO "ws2_32")
    add_test(NAME GgpoUdpValidation COMMAND GgpoUdpValidationTest)
    set_tests_properties(GgpoUdpValidation PROPERTIES TIMEOUT 15)
    add_executable(GgpoSpectatorCapacityTest "src/tests/ggpo_spectator_capacity_test.cxx")
    target_link_libraries(GgpoSpectatorCapacityTest PRIVATE ggpo::GGPO "ws2_32")
    add_test(NAME GgpoSpectatorCapacity COMMAND GgpoSpectatorCapacityTest)
    set_tests_properties(GgpoSpectatorCapacity PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "GGPO spectator capacity passed")
    add_executable(GgpoConfirmedFrameTest "src/tests/ggpo_confirmed_frame_test.cxx")
    target_link_libraries(GgpoConfirmedFrameTest PRIVATE ggpo::GGPO "ws2_32")
    add_test(NAME GgpoConfirmedFrame COMMAND GgpoConfirmedFrameTest)
    set_tests_properties(GgpoConfirmedFrame PROPERTIES TIMEOUT 15)

    add_executable(RollbackDiagnosticsTest "src/tests/rollback_diagnostics_test.cxx")
    target_link_libraries(RollbackDiagnosticsTest PRIVATE sf4e_common)
    add_test(NAME RollbackDiagnostics COMMAND RollbackDiagnosticsTest)
    set_tests_properties(RollbackDiagnostics PROPERTIES TIMEOUT 15)

    add_executable(GgpoGateTest "src/tests/ggpo_gate_test.cxx")
    target_link_libraries(GgpoGateTest PRIVATE sf4e_common)
    add_test(NAME GgpoGate COMMAND GgpoGateTest)
    set_tests_properties(GgpoGate PROPERTIES TIMEOUT 15)

    add_executable(PacingControllerTest "src/tests/pacing_controller_test.cxx")
    add_test(NAME PacingController COMMAND PacingControllerTest)
    set_tests_properties(PacingController PROPERTIES TIMEOUT 15)

    add_executable(StateHashTest "src/tests/state_hash_test.cxx")
    add_test(NAME StateHash COMMAND StateHashTest)
    set_tests_properties(StateHash PROPERTIES TIMEOUT 15)

    add_executable(SaveStateOwnershipTest "src/tests/savestate_ownership_test.cxx")
    add_test(NAME SaveStateOwnership COMMAND SaveStateOwnershipTest)
    set_tests_properties(SaveStateOwnership PROPERTIES TIMEOUT 15)
endif()

add_library(sf4e_app_services STATIC src/platform/ApplicationServices.cxx src/launcher/update/github_release_client.cxx)
target_compile_features(sf4e_app_services PUBLIC cxx_std_17)
target_compile_definitions(sf4e_app_services PRIVATE SF4E_APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(sf4e_app_services PUBLIC sf4e_settings PRIVATE sf4e_common sf4e_selection spdlog::spdlog nlohmann_json::nlohmann_json WIL::WIL pathcch shell32 shlwapi bcrypt)
if(BUILD_TESTING)
    add_executable(ApplicationServicesTest src/tests/application_services_test.cxx)
    target_link_libraries(ApplicationServicesTest PRIVATE sf4e_app_services)
    add_test(NAME ApplicationServices COMMAND ApplicationServicesTest)
    add_executable(EmberUpdateTest src/tests/ember_update_test.cxx)
    target_link_libraries(EmberUpdateTest PRIVATE sf4e_app_services nlohmann_json::nlohmann_json)
    add_test(NAME EmberUpdate COMMAND EmberUpdateTest)
endif()
target_link_libraries(sf4e PRIVATE sf4e_app_services)
add_library(sf4e_recovery STATIC src/ui/RecoverySurface.cxx)
target_compile_features(sf4e_recovery PUBLIC cxx_std_17)
target_link_libraries(sf4e_recovery PUBLIC sf4e_ui sf4e_app_services PRIVATE d3d9 ole32 shell32)
add_executable(Launcher WIN32 src/launcher/launcher.cxx src/launcher/netplay/netplay_persist.cxx)
target_link_libraries(Launcher PRIVATE sf4e_helper_platform sf4e_settings sf4e_recovery)
set(_SF4E_LAUNCHER_EXE_TARGET Launcher)
target_include_directories(${_SF4E_LAUNCHER_EXE_TARGET} PRIVATE ${VALVEFILEVDF_INCLUDE_DIRS} "src/common" "src/launcher")
		target_link_libraries(${_SF4E_LAUNCHER_EXE_TARGET} PRIVATE
    ${LIB_DETOURS}
    CLI11::CLI11
    spdlog::spdlog
    sf4e_common
    nlohmann_json::nlohmann_json
    WIL::WIL
    "shlwapi"
    "pathcch"
    "bcrypt"
    "shell32"
    "ole32"
    "oleaut32"
    "user32"
    "gdi32"
    "ws2_32"
)

add_library(Sidecar SHARED src/sidecar/sidecar.cxx)
target_include_directories(Sidecar PRIVATE ${VALVEFILEVDF_INCLUDE_DIRS})
target_link_libraries(Sidecar PRIVATE ${LIB_DETOURS} Dimps sf4e)
sf4e_configure_windows_version(Sidecar "SF4 Ember Netplay Sidecar" "Sidecar.dll" "VFT_DLL")
sf4e_embed_manifest(Sidecar "Sidecar.dll.manifest")

target_compile_definitions(Launcher PRIVATE -D_UNICODE -DUNICODE "SF4E_APP_VERSION=\"${PROJECT_VERSION}\"")
set_target_properties(Launcher PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
sf4e_configure_windows_version(Launcher "SF4 Ember Netplay" "Launcher.exe" "VFT_APP")
sf4e_embed_manifest(Launcher "Launcher.exe.manifest")
add_executable(Updater
    src/launcher/updater.cxx
)
add_library(sf4e_package_installer STATIC src/launcher/update/PackageInstaller.cxx)
target_compile_features(sf4e_package_installer PUBLIC cxx_std_17)
target_link_libraries(sf4e_package_installer PUBLIC sf4e_selection)
if(BUILD_TESTING)
    add_executable(OverlayPresentationTest src/tests/overlay_presentation_test.cxx)
    target_compile_features(OverlayPresentationTest PRIVATE cxx_std_17)
    add_test(NAME OverlayPresentation COMMAND OverlayPresentationTest)
    add_executable(PackageInstallerTest src/tests/package_installer_test.cxx)
    target_link_libraries(PackageInstallerTest PRIVATE sf4e_package_installer)
    add_test(NAME PackageInstaller COMMAND PackageInstallerTest)
endif()
target_link_libraries(Updater PRIVATE sf4e_package_installer sf4e_selection "pathcch" "shell32")
set_target_properties(Updater PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
sf4e_configure_windows_version(Updater "SF4 Ember Netplay Updater" "Updater.exe" "VFT_APP")

install(
  TARGETS Launcher Sidecar Updater
  RUNTIME DESTINATION "."
)
install(FILES "src/ui/fonts/OFL.txt" DESTINATION "notices" RENAME "Inter-OFL.txt")
install(FILES "src/ui/backends/LICENSE.txt" DESTINATION "notices" RENAME "Dear-ImGui-MIT.txt")
install(DIRECTORY "assets/selection" DESTINATION "assets" COMPONENT SelectionReferences OPTIONAL
    FILES_MATCHING PATTERN "*.png" PATTERN "*.jpg" PATTERN "*.json" PATTERN "*.md")
# Keep local builds self-contained when references have been fetched. Copy on
# each build because image-only changes do not cause Sidecar to relink.
add_custom_target(SelectionAssets
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/selection" "$<TARGET_FILE_DIR:Sidecar>/assets/selection"
    VERBATIM)
add_dependencies(Sidecar SelectionAssets)
add_custom_command(TARGET Sidecar POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Sidecar>/notices"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/fonts/OFL.txt"
        "$<TARGET_FILE_DIR:Sidecar>/notices/Inter-OFL.txt"
    VERBATIM)

set(SF4E_RUNTIME_DLLS
  spdlog.dll
  fmt.dll
  GGPO.dll
  zlib1.dll
)
foreach(_dll IN LISTS SF4E_RUNTIME_DLLS)
  install(
    FILES "$<TARGET_FILE_DIR:Sidecar>/${_dll}"
    DESTINATION "."
    OPTIONAL
  )
endforeach()

set(CPACK_PACKAGE_NAME "sf4-ember-netplay")
set(CPACK_PACKAGE_VENDOR "The ${PROJECT_NAME} Dev Team")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_NAME} ${PROJECT_DESCRIPTION}")
