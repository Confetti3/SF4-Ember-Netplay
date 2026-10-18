cmake_minimum_required (VERSION 3.26)

include("${CMAKE_SOURCE_DIR}/cmake/sf4e_version.cmake")

project (
    sf4e
    VERSION 1.4.0
    DESCRIPTION "SF4 Ember Netplay: private rollback rooms and an in-game overlay for Ultra Street Fighter IV."
    HOMEPAGE_URL "https://github.com/Confetti3/SF4-Ember-Netplay"
)

include(CTest)
include("${CMAKE_SOURCE_DIR}/cmake/sf4e_tests.cmake")

option(SF4E_CORE_TESTS_ONLY "Build game-independent migration tests without Windows/game dependencies" OFF)
add_library(sf4e_netplay_core STATIC "src/netplay/SessionController.cxx" "src/netplay/MatchResultOutbox.cxx")
add_library(sf4e_selection STATIC "src/common/FighterCatalog.cxx" "src/common/StageCatalog.cxx" "src/common/InstalledContent.cxx")
target_compile_features(sf4e_selection PUBLIC cxx_std_11)
target_compile_features(sf4e_netplay_core PUBLIC cxx_std_11)
# Durable create-only and replacing file writes (platform/DurableFile.hxx), for
# replays, settings and the updater's journal. Its bounded reader is header only.
if(WIN32)
    add_library(sf4e_durable_file STATIC "src/platform/DurableFile.cxx")
    target_compile_features(sf4e_durable_file PUBLIC cxx_std_17)
    add_library(sf4e_custom_content_files STATIC "src/platform/CustomContentFiles.cxx")
    target_link_libraries(sf4e_custom_content_files PUBLIC sf4e_selection)
endif()
if(BUILD_TESTING)
    if(WIN32)
        sf4e_add_unit_test(NAME CustomContentFiles SOURCES "src/tests/custom_content_files_test.cxx"
            STD 17 LIBS sf4e_custom_content_files TIMEOUT 15)
    endif()
    sf4e_add_unit_test(NAME NativeRepairTiming SOURCES "src/tests/native_repair_timing_test.cxx" STD 17)
    find_package(Threads REQUIRED)
    sf4e_add_unit_test(NAME TrainingSession SOURCES "src/tests/training_session_test.cxx" STD 14)
    sf4e_add_unit_test(NAME FrameMeterRun SOURCES "src/tests/frame_meter_run_test.cxx" STD 14)
    sf4e_add_unit_test(NAME FocusCancel SOURCES "src/tests/focus_cancel_test.cxx" STD 14)
    sf4e_add_unit_test(NAME TrainingReply SOURCES "src/tests/training_reply_test.cxx" STD 14)
    sf4e_add_unit_test(NAME TrainingFacing SOURCES "src/tests/training_facing_test.cxx" STD 14)
    # The bank's test includes RecordingBank.hxx alone, so it fails to build
    # if the bank comes to need the session.
    sf4e_add_unit_test(NAME TrainingRecordingBank SOURCES "src/tests/training_recording_bank_test.cxx" STD 14)
    sf4e_add_unit_test(NAME TrainingRecording SOURCES "src/tests/training_recording_test.cxx" STD 14)
    sf4e_add_unit_test(NAME TrainingPendingReply SOURCES "src/tests/training_pending_reply_test.cxx" STD 14)
    sf4e_add_unit_test(NAME TrainingPadStandby SOURCES "src/tests/training_pad_standby_test.cxx" STD 14)
    sf4e_add_unit_test(NAME FighterCatalog SOURCES "src/tests/fighter_catalog_test.cxx" LIBS sf4e_selection TIMEOUT 15)
    sf4e_add_unit_test(NAME SessionController SOURCES "src/tests/session_controller_test.cxx" LIBS sf4e_netplay_core Threads::Threads TIMEOUT 15)
    sf4e_add_unit_test(NAME RollbackHud SOURCES "src/tests/rollback_hud_test.cxx")
    sf4e_add_unit_test(NAME HudMementoBounds SOURCES "src/tests/hud_memento_bounds_test.cxx")
    sf4e_add_unit_test(NAME VfxObjectMemento SOURCES "src/tests/vfx_object_memento_test.cxx")
    sf4e_add_unit_test(NAME FocusGate SOURCES "src/tests/focus_gate_test.cxx")
    sf4e_add_unit_test(NAME AwayAlert SOURCES "src/tests/away_alert_test.cxx")
    sf4e_add_unit_test(NAME InputLagControl SOURCES "src/tests/input_lag_control_test.cxx")
    # The game's object layout holds 32-bit pointers.
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        sf4e_add_unit_test(NAME KeyboardSlots SOURCES "src/tests/keyboard_slots_test.cxx")
    endif()
    sf4e_add_unit_test(NAME MatchFrameMeter SOURCES "src/tests/match_frame_meter_test.cxx")
    sf4e_add_unit_test(NAME GfxActionPool SOURCES "src/tests/gfx_action_pool_test.cxx" STD 14)
    sf4e_add_unit_test(NAME SoundReconcile SOURCES "src/tests/sound_reconcile_test.cxx")
    sf4e_add_unit_test(NAME ReusableRecords SOURCES "src/tests/reusable_records_test.cxx")
    sf4e_add_unit_test(NAME NativeMatchResult SOURCES "src/tests/native_match_result_test.cxx" STD 11 TIMEOUT 15)
    sf4e_add_unit_test(NAME UiPolish SOURCES "src/tests/ui-polish/ui_polish_test.cxx" STD 14 TIMEOUT 10)
    sf4e_add_unit_test(NAME GgpoGate SOURCES "src/tests/ggpo_gate_test.cxx" "src/common/sf4e__GgpoGate.cxx" STD 11 TIMEOUT 15)
    sf4e_add_unit_test(NAME CrashReport SOURCES "src/tests/crash_report_test.cxx" STD 11 TIMEOUT 15)
    sf4e_add_unit_test(NAME HelperErrorScope SOURCES "src/tests/helper_error_scope_test.cxx" STD 11 TIMEOUT 15)
    sf4e_add_unit_test(NAME RecoveryProgress SOURCES "src/tests/recovery_progress_test.cxx" STD 17 TIMEOUT 15)
    sf4e_add_unit_test(NAME JoinLink SOURCES "src/tests/join_link_test.cxx" STD 11 TIMEOUT 15)
    sf4e_add_unit_test(NAME RollbackAudit SOURCES "src/tests/rollback_audit_test.cxx" STD 14 TIMEOUT 15)
    sf4e_add_unit_test(NAME AfterimageMemento SOURCES "src/tests/afterimage_memento_test.cxx" STD 14 TIMEOUT 15)
    sf4e_add_unit_test(NAME MementoGuards SOURCES "src/tests/memento_guards_test.cxx" STD 14 TIMEOUT 15)
    sf4e_add_unit_test(NAME PacingController SOURCES "src/tests/pacing_controller_test.cxx" STD 11 TIMEOUT 15)
    # The cache the Discord SDK is unpacked into (cmake/sf4e_verified_unpack.cmake).
    add_test(NAME VerifiedUnpack COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DWORK=${CMAKE_BINARY_DIR}/verified-unpack-test" -P "${CMAKE_SOURCE_DIR}/src/tests/verified_unpack_test.cmake")
    set_tests_properties(VerifiedUnpack PROPERTIES TIMEOUT 30)
    # The public room soak's verdict (src/tests/soak), on made-up rooms.
    sf4e_add_unit_test(NAME SoakVerdict SOURCES "src/tests/soak/soak_verdict_test.cxx" "src/tests/soak/soak_verdict.cxx" STD 17 TIMEOUT 15)
    sf4e_add_unit_test(NAME InputDelay SOURCES "src/tests/input_delay_test.cxx" STD 11 TIMEOUT 15)
    sf4e_add_unit_test(NAME AutoDelayCheck SOURCES "src/tests/auto_delay_check_test.cxx" STD 11 TIMEOUT 15)
    sf4e_add_unit_test(NAME GamePing SOURCES "src/tests/game_ping_test.cxx" STD 11 TIMEOUT 15)
    sf4e_add_unit_test(NAME ReplaySlots SOURCES "src/tests/replay_slots_test.cxx" STD 17 TIMEOUT 15)
    sf4e_add_unit_test(NAME ReplayFiles SOURCES "src/tests/replay_files_test.cxx" STD 17
        LIBS $<$<PLATFORM_ID:Windows>:sf4e_durable_file> TIMEOUT 15)
    sf4e_add_unit_test(NAME ReplayArchiveIndex SOURCES "src/tests/replay_archive_index_test.cxx" STD 17
        LIBS Threads::Threads $<$<PLATFORM_ID:Windows>:bcrypt> TIMEOUT 15)
    sf4e_add_unit_test(NAME ReplayArchivePass SOURCES "src/tests/replay_archive_pass_test.cxx" STD 17 LIBS Threads::Threads TIMEOUT 60)
    sf4e_add_unit_test(NAME ReplayImportWorker SOURCES "src/tests/replay_import_worker_test.cxx" STD 17 LIBS Threads::Threads TIMEOUT 15)
    sf4e_add_unit_test(NAME ReplayImportHandoff SOURCES "src/tests/replay_import_handoff_test.cxx" STD 17 LIBS Threads::Threads TIMEOUT 15)
    sf4e_add_unit_test(NAME ReplayProvenance SOURCES "src/tests/replay_provenance_test.cxx" STD 17 LIBS Threads::Threads TIMEOUT 15)
    sf4e_add_unit_test(NAME ReplayInputs SOURCES "src/tests/replay_inputs_test.cxx" STD 17 TIMEOUT 15)
    sf4e_add_unit_test(NAME ReplayTransport SOURCES "src/tests/replay_transport_test.cxx" STD 14 TIMEOUT 15)
    sf4e_add_unit_test(NAME ReplayInputLane SOURCES "src/tests/replay_input_lane_test.cxx" STD 17 TIMEOUT 15)
    # The recorder layout is the 32-bit engine's.
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        sf4e_add_unit_test(NAME ReplayRecorder SOURCES "src/tests/replay_recorder_test.cxx" STD 14 TIMEOUT 15)
    endif()
endif()
if(SF4E_CORE_TESTS_ONLY)
    return()
endif()
# After the core-only return: on Windows the helper needs Cargo.
include("${CMAKE_SOURCE_DIR}/cmake/sf4e_helper.cmake")

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
    sf4e_add_unit_test(NAME CheckpointTransfer SOURCES "src/tests/checkpoint_transfer_test.cxx" STD 17 LIBS nlohmann_json::nlohmann_json bcrypt)
    sf4e_add_unit_test(NAME RoomLoadWindow SOURCES "src/tests/room_load_window_test.cxx" STD 17 LIBS nlohmann_json::nlohmann_json)
endif()
include("${CMAKE_SOURCE_DIR}/cmake/sf4e_discord.cmake")
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(wil CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
if(BUILD_TESTING)
    # Links Sidecar's own crash record, so the test runs the production handler.
    # It needs spdlog and GGPO, so it stays out of the core-only build.
    add_executable(HeapCorruptionCaptureTest "src/tests/heap_corruption_capture_test.cxx" "src/sf4e/sf4e__CrashDiagnostics.cxx" "src/sf4e/sf4e__ImportPatch.cxx")
    target_compile_features(HeapCorruptionCaptureTest PRIVATE cxx_std_17)
    target_link_libraries(HeapCorruptionCaptureTest PRIVATE spdlog::spdlog ggpo::GGPO "dbghelp" "psapi" "shlwapi")
    add_test(NAME HeapCorruptionCapture COMMAND HeapCorruptionCaptureTest)
    add_test(NAME HeapCheckSettings COMMAND HeapCorruptionCaptureTest heap-check-settings)
    add_test(NAME HeapCheckEnvironment COMMAND HeapCorruptionCaptureTest heap-check-environment)
    set_tests_properties(HeapCorruptionCapture PROPERTIES TIMEOUT 90)
endif()

add_library(sf4e_room_model STATIC "src/session/RoomModel.cxx" "src/session/RoomModelActions.cxx" "src/session/RoomModelJson.cxx" "src/session/RoomModelTournament.cxx" "src/session/RoomModelServerOwned.cxx" "src/session/RoomDigest.cxx")
target_compile_features(sf4e_room_model PUBLIC cxx_std_14)
target_link_libraries(sf4e_room_model PUBLIC nlohmann_json::nlohmann_json "bcrypt")
# The game's Steam client through steam_api.dll (profile overlay, remote storage).
add_library(sf4e_steam STATIC "src/sf4e/sf4e__Steam.cxx")
target_compile_features(sf4e_steam PUBLIC cxx_std_11)
target_link_libraries(sf4e_steam PUBLIC spdlog::spdlog)
if(BUILD_TESTING)
    sf4e_add_unit_test(NAME SteamId SOURCES "src/tests/steam_id_test.cxx" TIMEOUT 15)
    sf4e_add_unit_test(NAME SteamAdapter SOURCES "src/tests/steam_adapter_test.cxx" LIBS sf4e_steam TIMEOUT 15)
endif()
# Playing a bridge-run tournament match: a pure state machine the runtime ticks.
add_library(sf4e_tournament_play STATIC "src/netplay/TournamentPlay.cxx")
target_compile_features(sf4e_tournament_play PUBLIC cxx_std_17)
target_link_libraries(sf4e_tournament_play PUBLIC sf4e_room_model)
if(BUILD_TESTING)
    sf4e_add_unit_test(NAME RoomMessageQueue SOURCES "src/tests/room_message_queue_test.cxx" LIBS sf4e_room_model)
    sf4e_add_unit_test(NAME MatchResultOutbox SOURCES "src/tests/match_result_outbox_test.cxx" LIBS sf4e_netplay_core sf4e_room_model TIMEOUT 15)
    sf4e_add_unit_test(NAME ParkedIntent SOURCES "src/tests/parked_intent_test.cxx" TIMEOUT 15)
endif()
if(BUILD_TESTING)
    sf4e_add_unit_test(NAME DiscordPresence SOURCES src/tests/discord_presence_test.cxx LIBS sf4e_room_model)
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
    sf4e_add_unit_test(NAME RoomAuthority SOURCES "src/tests/room_authority_test.cxx" LIBS sf4e_room_model TIMEOUT 30)
    sf4e_add_unit_test(NAME RoomDelayLimits SOURCES "src/tests/room_delay_limits_test.cxx" LIBS sf4e_room_model TIMEOUT 30)
    sf4e_add_unit_test(NAME RoomSnapshot SOURCES "src/tests/room_snapshot_test.cxx" LIBS sf4e_room_model TIMEOUT 30)
    sf4e_add_unit_test(NAME RoomSteamProfile SOURCES "src/tests/room_steam_profile_test.cxx" LIBS sf4e_room_model TIMEOUT 30)
    sf4e_add_unit_test(NAME RoomCountry SOURCES "src/tests/room_country_test.cxx" LIBS sf4e_room_model TIMEOUT 30)
    sf4e_add_unit_test(NAME RoomSets SOURCES "src/tests/room_sets_test.cxx" LIBS sf4e_room_model TIMEOUT 30)
    sf4e_add_unit_test(NAME RoomTournament SOURCES "src/tests/room_tournament_test.cxx" LIBS sf4e_room_model)
    sf4e_add_unit_test(NAME TournamentPlay SOURCES "src/tests/tournament_play_test.cxx" LIBS sf4e_tournament_play)
    sf4e_add_unit_test(NAME IdentityEvents SOURCES "src/tests/identity_events_test.cxx" "src/session/IdentityEvents.cxx"
        LIBS nlohmann_json::nlohmann_json STD 17)
    sf4e_add_unit_test(NAME TournamentFuzz SOURCES "src/tests/tournament_fuzz_test.cxx" "src/session/TournamentAnswers.cxx"
        LIBS sf4e_tournament_play nlohmann_json::nlohmann_json TIMEOUT 300)
    sf4e_add_unit_test(NAME RoomLifecycle SOURCES "src/tests/room_lifecycle_test.cxx" LIBS sf4e_room_model TIMEOUT 30)
    sf4e_add_unit_test(NAME RoomServerOwned SOURCES "src/tests/room_server_owned_test.cxx" LIBS sf4e_room_model TIMEOUT 30)
    sf4e_add_unit_test(NAME RoomPrivateGolden SOURCES "src/tests/room_private_golden_test.cxx" LIBS sf4e_room_model)
    sf4e_add_unit_test(NAME RoomSpectatorLock SOURCES "src/tests/room_spectator_lock_test.cxx" LIBS sf4e_room_model TIMEOUT 30)
    sf4e_add_unit_test(NAME RoomReadyTimeout SOURCES "src/tests/room_ready_timeout_test.cxx" LIBS sf4e_room_model TIMEOUT 30)
    sf4e_add_unit_test(NAME CreatedRules SOURCES "src/tests/created_rules_test.cxx" LIBS sf4e_room_model TIMEOUT 30)
    sf4e_add_unit_test(NAME ReadyChime SOURCES "src/tests/ready_chime_test.cxx" LIBS sf4e_room_model TIMEOUT 30)
    sf4e_add_unit_test(NAME TrainingCall SOURCES "src/tests/training_call_test.cxx" LIBS sf4e_room_model TIMEOUT 30)
    sf4e_add_unit_test(NAME RoomTrainingFlag SOURCES "src/tests/room_training_flag_test.cxx" LIBS sf4e_room_model TIMEOUT 30)
    add_executable(SessionRecoveryGateTest "src/tests/session_recovery_gate_test.cxx")
    target_compile_features(SessionRecoveryGateTest PRIVATE cxx_std_14)
    target_link_libraries(SessionRecoveryGateTest PRIVATE sf4e_room_model)
    if(MSVC)
        target_compile_options(SessionRecoveryGateTest PRIVATE /UNDEBUG)
    else()
        target_compile_options(SessionRecoveryGateTest PRIVATE -UNDEBUG)
    endif()
    add_test(NAME SessionRecoveryGate COMMAND SessionRecoveryGateTest)
    sf4e_add_unit_test(NAME CheckpointDecodeWorker SOURCES "src/tests/checkpoint_decode_worker_test.cxx" STD 14 LIBS sf4e_room_model TIMEOUT 30)
    sf4e_add_unit_test(NAME SpectatorPolicy SOURCES "src/tests/spectator_policy_test.cxx")
    sf4e_add_unit_test(NAME NextBattleRequest SOURCES "src/tests/next_battle_request_test.cxx" STD 14 TIMEOUT 15)
    sf4e_add_unit_test(NAME SpectatorSync SOURCES "src/tests/spectator_sync_test.cxx" STD 14 LIBS sf4e_room_model TIMEOUT 15)
    sf4e_add_unit_test(NAME GameDisplayConfig SOURCES "src/tests/game_display_config_test.cxx" STD 17)
    sf4e_add_unit_test(NAME FrameRateWindow SOURCES "src/tests/frame_rate_window_test.cxx" STD 17)
endif()

if(BUILD_TESTING)
    sf4e_add_unit_test(NAME SelectionValue SOURCES "src/tests/selection_value_test.cxx" LIBS nlohmann_json::nlohmann_json TIMEOUT 15)
    sf4e_add_unit_test(NAME MoveNotation SOURCES "src/tests/move_notation_test.cxx" STD 14 LIBS nlohmann_json::nlohmann_json TIMEOUT 15)
    sf4e_add_unit_test(NAME BacFile SOURCES "src/tests/bac_file_test.cxx" STD 14 TIMEOUT 15)
    sf4e_add_unit_test(NAME PracticeSettings SOURCES "src/tests/practice_settings_test.cxx" STD 14 LIBS nlohmann_json::nlohmann_json TIMEOUT 15)
    sf4e_add_unit_test(NAME TrainingDummyControl SOURCES "src/tests/training_dummy_control_test.cxx" STD 14 LIBS nlohmann_json::nlohmann_json)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sf4e_locales.cmake")
add_library(sf4e_localization STATIC
    "src/common/Localization.cxx"
    "src/common/GameLanguage.cxx"
    "src/common/SteamKeyValues.cxx"
    "src/platform/LocaleWindows.cxx")
target_compile_features(sf4e_localization PUBLIC cxx_std_17)
target_include_directories(sf4e_localization PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
    PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated/public")
target_link_libraries(sf4e_localization PUBLIC fmt::fmt PRIVATE "advapi32")
if(BUILD_TESTING)
    # imgui for the glyph ranges the catalog coverage check reads, headers only.
    add_executable(LocalizationTest "src/tests/localization_test.cxx")
    target_link_libraries(LocalizationTest PRIVATE sf4e_localization imgui::imgui)
    # EmbeddedFonts.hxx: coverage is checked against the font bytes that ship.
    target_include_directories(LocalizationTest PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_test(NAME Localization COMMAND LocalizationTest "${CMAKE_SOURCE_DIR}")
    sf4e_add_unit_test(NAME GameLanguage SOURCES "src/tests/game_language_test.cxx" LIBS sf4e_localization TIMEOUT 15)
    # The country table against the flag atlas, its index, the catalogs and the
    # package inventory, and the copies a build stages beside Sidecar.dll.
    add_executable(CountriesTest "src/tests/countries_test.cxx")
    target_link_libraries(CountriesTest PRIVATE sf4e_localization nlohmann_json::nlohmann_json)
    add_test(NAME Countries COMMAND CountriesTest "${CMAKE_SOURCE_DIR}" "$<TARGET_FILE_DIR:Sidecar>")
endif()

# The UI preference store is a settings store that validates one localization
# value, so it lives with the settings and depends on localization, not both
# ways around.
add_library(sf4e_settings STATIC "src/netplay/JsonFileStore.cxx" "src/netplay/SettingsStore.cxx"
    "src/netplay/SettingsWriter.cxx" "src/platform/UiPreferencesStore.cxx"
    "src/platform/GameDisplaySettings.cxx")
target_compile_features(sf4e_settings PUBLIC cxx_std_17)
target_link_libraries(sf4e_settings PUBLIC nlohmann_json::nlohmann_json sf4e_localization PRIVATE sf4e_durable_file "shell32" "ole32")
if(BUILD_TESTING)
    # It takes 2 s alone. Its saves are flushed writes, which take up to ten
    # times as long while other tests and builds use the disk.
    sf4e_add_unit_test(NAME SettingsStore SOURCES "src/tests/settings_store_test.cxx" LIBS sf4e_settings sf4e_durable_file TIMEOUT 60)
    sf4e_add_unit_test(NAME UiPreferencesStore SOURCES "src/tests/ui_preferences_store_test.cxx" LIBS sf4e_settings)
    sf4e_add_unit_test(NAME RecordingFileStore SOURCES "src/tests/recording_file_store_test.cxx" LIBS sf4e_settings sf4e_durable_file TIMEOUT 60)
endif()

add_library(sf4e_ui STATIC "src/ui/ApplicationShell.cxx" "src/ui/ControllersScreen.cxx" "src/ui/ReplaysPanel.cxx" "src/ui/RoomPanel.cxx" "src/ui/RoomControls.cxx" "src/ui/Theme.cxx" "src/ui/MatchHud.cxx" "src/ui/SelectionArt.cxx" "src/ui/FighterSelector.cxx")
target_sources(sf4e_ui PRIVATE src/ui/Win32Input.cxx src/ui/Win32InputBridge.cxx)
target_sources(sf4e_ui PRIVATE src/ui/GameMenu.cxx src/ui/MenuDialogs.cxx src/ui/ChatTranscript.cxx src/ui/ChatPanel.cxx)
install(DIRECTORY "assets/brand" DESTINATION "assets" FILES_MATCHING PATTERN "*.png")
install(DIRECTORY "assets/input" DESTINATION "assets" FILES_MATCHING PATTERN "*.png" PATTERN "*.txt")
install(DIRECTORY "assets/flags" DESTINATION "assets" FILES_MATCHING PATTERN "flags.png" PATTERN "flags.json" PATTERN "LICENSE" PATTERN "README.md")
target_sources(sf4e_ui PRIVATE src/ui/ControllerNavigation.cxx src/ui/backends/imgui_impl_win32.cpp)
set_source_files_properties(src/ui/backends/imgui_impl_win32.cpp PROPERTIES COMPILE_DEFINITIONS IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
target_sources(sf4e_ui PRIVATE src/ui/TrainingPanel.cxx src/ui/TrainingMeter.cxx src/ui/TrainingShortcuts.cxx src/ui/IdentityPanel.cxx src/ui/IdentityConnect.cxx src/ui/IdentitySignIn.cxx src/ui/IdentityMatches.cxx src/ui/PublicRoomsPanel.cxx src/ui/PublicRoomsCards.cxx src/ui/PublicRoomsSetup.cxx src/ui/MatchAnnouncements.cxx)
target_sources(sf4e_ui PRIVATE src/ui/OverlayLayers.cxx src/ui/ReplayPlaybackHud.cxx src/ui/TrainingInRoom.cxx)
target_compile_features(sf4e_ui PUBLIC cxx_std_17)
target_sources(sf4e_ui PRIVATE src/ui/ProblemReportPanel.cxx src/ui/PadSetupScreen.cxx src/ui/ApplicationShell__PadSetup.cxx src/ui/ButtonLayoutScreen.cxx src/ui/ApplicationShell__ButtonLayout.cxx src/ui/ApplicationShell__Create.cxx src/ui/ApplicationShell__Links.cxx src/ui/RoomCards.cxx src/ui/ApplicationShell__Updates.cxx src/ui/UpdatePrompt.cxx src/ui/CountryFlag.cxx src/ui/CountryPicker.cxx)
add_library(sf4e_problem_report STATIC src/platform/ProblemReport.cxx)
target_compile_features(sf4e_problem_report PUBLIC cxx_std_17)
target_link_libraries(sf4e_problem_report PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(sf4e_ui PUBLIC sf4e_problem_report)
if(BUILD_TESTING)
    sf4e_add_unit_test(NAME ProblemReport SOURCES "src/tests/problem_report_test.cxx" LIBS sf4e_problem_report TIMEOUT 30)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sf4e_fonts.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sf4e_brand.cmake")
target_link_libraries(sf4e_ui PUBLIC imgui::imgui sf4e_selection sf4e_room_model sf4e_localization sf4e_settings PRIVATE ZLIB::ZLIB "windowscodecs" "ole32" "d3d9")
target_compile_definitions(sf4e_ui PRIVATE SF4E_BUILD_VERSION="${PROJECT_VERSION}")
if(BUILD_TESTING)
    # The scenario fragments' non-template parts compile on their own; the
    # scenarios still run in the order ui_render_test.cxx calls them.
    add_executable(UiRenderTest "src/tests/ui_render_test.cxx" "src/tests/ui_render_match_hud.cxx" "src/tests/ui_render_overlay.cxx")
    target_compile_definitions(UiRenderTest PRIVATE SF4E_TEST_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets/selection")
    sf4e_add_unit_test(NAME ControllerNavigation SOURCES src/tests/controller_navigation_test.cxx LIBS sf4e_ui Dimps TIMEOUT 30)
    sf4e_add_unit_test(NAME MenuButtons SOURCES src/tests/menu_buttons_test.cxx LIBS sf4e_ui Dimps sf4e_settings STD 17)
    sf4e_add_unit_test(NAME GamepadPrompts SOURCES src/tests/gamepad_prompts_test.cxx LIBS sf4e_ui STD 17)
    sf4e_add_unit_test(NAME ButtonLayout SOURCES src/tests/button_layout_test.cxx LIBS sf4e_settings STD 17)
    sf4e_add_unit_test(NAME PadSetup SOURCES src/tests/pad_setup_test.cxx STD 17)
    sf4e_add_unit_test(NAME PadSetupMapping SOURCES src/tests/pad_setup_mapping_test.cxx LIBS SDL3::SDL3-static sf4e_settings STD 17)
    sf4e_add_unit_test(NAME GamepadSlots SOURCES src/tests/gamepad_slots_test.cxx STD 17)
    sf4e_add_unit_test(NAME GamepadRumble SOURCES src/tests/gamepad_rumble_test.cxx STD 17)
    sf4e_add_unit_test(NAME ControllersScreen SOURCES src/tests/controllers_screen_test.cxx LIBS sf4e_ui STD 17)
    sf4e_add_unit_test(NAME GamepadSlotsImportPatch SOURCES src/tests/import_patch_test.cxx src/sf4e/sf4e__ImportPatch.cxx STD 17)
    sf4e_add_unit_test(NAME DirectInputFilter SOURCES src/tests/directinput_filter_test.cxx LIBS sf4e dxguid STD 17)
    sf4e_add_unit_test(NAME ShellJourney SOURCES src/tests/shell_journey_test.cxx src/tests/shell_replay_journey_test.cxx
        src/tests/shell_room_journey_test.cxx src/tests/shell_selection_journey_test.cxx
        src/tests/shell_chat_journey_test.cxx src/tests/shell_training_journey_test.cxx src/tests/shell_prompt_journey_test.cxx src/tests/shell_controller_journey_test.cxx src/tests/shell_button_layout_journey_test.cxx
        src/tests/shell_feedback_journey_test.cxx src/tests/shell_selector_journey_test.cxx src/tests/shell_update_journey_test.cxx
        LIBS sf4e_ui Dimps TIMEOUT 30)
    sf4e_add_unit_test(NAME IdentityJourney SOURCES src/tests/identity_journey_test.cxx src/tests/discord_connect_journey_test.cxx
        src/tests/builtin_service_journey_test.cxx src/tests/challonge_connect_journey_test.cxx
        LIBS sf4e_ui Dimps TIMEOUT 30)
    sf4e_add_unit_test(NAME PublicRoomsJourney SOURCES src/tests/public_rooms_journey_test.cxx src/tests/public_rooms_setup_journey_test.cxx
        LIBS sf4e_ui Dimps TIMEOUT 30)
    sf4e_add_unit_test(NAME RoomPanelNavigation SOURCES src/tests/room_panel_navigation_test.cxx LIBS sf4e_ui Dimps TIMEOUT 30)
    sf4e_add_unit_test(NAME WatchedMatchExit SOURCES src/tests/watched_match_exit_test.cxx LIBS sf4e_ui Dimps TIMEOUT 30)
    sf4e_add_unit_test(NAME LatencyMark SOURCES src/tests/latency_mark_test.cxx LIBS sf4e_ui Dimps TIMEOUT 15)
    sf4e_add_unit_test(NAME ReplayScreenText SOURCES src/tests/replay_screen_text_test.cxx LIBS sf4e_ui Dimps TIMEOUT 15)
    sf4e_add_unit_test(NAME NetworkFeedback SOURCES src/tests/network_feedback_test.cxx LIBS sf4e_ui Dimps TIMEOUT 30)
    sf4e_add_unit_test(NAME NativeSelectionAvailability SOURCES src/tests/native_selection_availability_test.cxx LIBS sf4e_ui Dimps TIMEOUT 30)
    sf4e_add_unit_test(NAME WineBuiltin SOURCES src/tests/wine_builtin_test.cxx STD 17)
    sf4e_add_unit_test(NAME Win32Cursor SOURCES src/tests/win32_cursor_test.cxx LIBS sf4e_ui)
    sf4e_add_unit_test(NAME CountryPicker SOURCES src/tests/country_picker_test.cxx LIBS sf4e_ui TIMEOUT 30)
    target_link_libraries(UiRenderTest PRIVATE sf4e_ui "d3d9" "dbghelp" "psapi")
    add_test(NAME UiRender COMMAND UiRenderTest)
    # Each sweep takes under a minute with the GPU transforming vertices. The
    # budgets cover an adapter without hardware transform, where the CPU does
    # it and a sweep takes several minutes.
    set_tests_properties(UiRender PROPERTIES TIMEOUT 600)
    # Each translation at three sizes, in its own run so neither sweep nears
    # its timeout as languages are added.
    add_test(NAME UiRenderLocales COMMAND UiRenderTest)
    set_tests_properties(UiRenderLocales PROPERTIES TIMEOUT 600 ENVIRONMENT "SF4E_UI_RENDER_LOCALES=translations")
    # The atlas rebuild on its own: every language and scale within the atlas
    # budget, then random player text through many rebuilds.
    add_test(NAME UiRenderAtlas COMMAND UiRenderTest)
    set_tests_properties(UiRenderAtlas PROPERTIES TIMEOUT 300 ENVIRONMENT "SF4E_UI_RENDER_ATLAS_STRESS=150")
    add_executable(SelectionArtTest src/tests/selection_art_test.cxx)
    target_compile_definitions(SelectionArtTest PRIVATE SF4E_TEST_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets/selection")
    target_link_libraries(SelectionArtTest PRIVATE sf4e_ui "d3d9")
    add_test(NAME SelectionArt COMMAND SelectionArtTest)
    set_tests_properties(SelectionArt PROPERTIES TIMEOUT 60)
endif()

add_library(Dimps STATIC
    "src/Dimps/Dimps__Selection.cxx"
    "src/Dimps/Dimps__Sound.hxx"
    "src/Dimps/Dimps__Sound.cxx"
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
    "src/common/NetworkLink.cxx"
    "src/common/NetworkLink.hxx"
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
include("${CMAKE_SOURCE_DIR}/cmake/sf4e_replays.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/sf4e_video.cmake")
if(BUILD_TESTING)
    sf4e_add_unit_test(NAME NetworkLink SOURCES "src/tests/network_link_test.cxx" LIBS sf4e_common TIMEOUT 30)
    sf4e_add_unit_test(NAME HttpPostCancel SOURCES "src/tests/http_post_cancel_test.cxx" STD 17 LIBS sf4e_common ws2_32 TIMEOUT 60)
endif()

add_library(sf4e STATIC
    "src/sf4e/sf4e__Gamepads.cxx"
    "src/sf4e/sf4e__GamepadLayouts.cxx"
    "src/sf4e/sf4e__OptionUIController.cxx"
    "src/sf4e/sf4e__DirectInputFilter.cxx"
    "src/sf4e/sf4e__ImportPatch.cxx"
    "src/sf4e/sf4e__InputDevices.cxx"
    "src/sf4e/sf4e.hxx"
    "src/sf4e/sf4e.cxx"
    "src/sf4e/sf4e__Event.hxx"
    "src/sf4e/sf4e__Event.cxx"
    "src/sf4e/sf4e__Game.hxx"
    "src/sf4e/sf4e__Game.cxx"
    "src/sf4e/sf4e__Game__Battle.hxx"
    "src/sf4e/sf4e__Game__Battle.cxx"
    "src/sf4e/sf4e__Game__Battle__Chara.hxx"
    "src/sf4e/sf4e__Game__Battle__Chara.cxx"
    "src/sf4e/sf4e__Game__Battle__Effect.hxx"
    "src/sf4e/sf4e__Game__Battle__Effect.cxx"
    "src/sf4e/sf4e__Game__Battle__Hud.hxx"
    "src/sf4e/sf4e__Game__Battle__Hud.cxx"
    "src/sf4e/sf4e__Game__Battle__System.hxx"
    "src/sf4e/sf4e__Game__Battle__System.cxx"
    "src/sf4e/sf4e__Game__Battle__System__FrameTrace.cxx"
    "src/sf4e/sf4e__Game__Battle__System__Ggpo.cxx"
    "src/sf4e/sf4e__Game__Battle__System__RollbackStress.cxx"
    "src/sf4e/sf4e__Game__Battle__System__SaveState.cxx"
    "src/sf4e/sf4e__MementoGuards.cxx"
    "src/sf4e/sf4e__Game__Battle__Vfx.hxx"
    "src/sf4e/sf4e__Game__Battle__Vfx.cxx"
    "src/sf4e/sf4e__GameEvents.hxx"
    "src/sf4e/sf4e__GameEvents.cxx"
    "src/sf4e/sf4e__Overlay.hxx"
    "src/sf4e/sf4e__Overlay.cxx"
    "src/training/TrainingRuntime.cxx"
    "src/training/TrainingCapture.cxx"
    "src/sf4e/sf4e__OverlayPrefs.hxx"
    "src/sf4e/sf4e__OverlayPrefs.cxx"
    "src/sf4e/sf4e__Pad.hxx"
    "src/sf4e/sf4e__Pad.cxx"
    "src/sf4e/sf4e__BackgroundPlay.hxx"
    "src/sf4e/sf4e__BackgroundPlay.cxx"
    "src/sf4e/sf4e__AwayAlert.hxx"
    "src/sf4e/sf4e__AwayAlert.cxx"
    "src/sf4e/sf4e__InputLag.hxx"
    "src/sf4e/sf4e__InputLag.cxx"
    "src/sf4e/sf4e__Platform.hxx"
    "src/sf4e/sf4e__Platform.cxx"
    "src/sf4e/sf4e__ReplayStore.hxx"
    "src/sf4e/sf4e__ReplayStore.cxx"
    "src/sf4e/sf4e__ReplayPlayback.hxx"
    "src/sf4e/sf4e__ReplayPlayback.cxx"
    "src/sf4e/sf4e__CrashDiagnostics.hxx"
    "src/sf4e/sf4e__CrashDiagnostics.cxx"
    "src/sf4e/sf4e__UserApp.hxx"
    "src/sf4e/sf4e__UserApp.cxx"
    "src/sf4e/sf4e__CustomContent.hxx"
    "src/sf4e/sf4e__CustomContent.cxx"
    "src/sf4e/sf4e__NetplayFacade.hxx"
    "src/sf4e/sf4e__NetplayFacade.cxx"
    "src/sf4e/sf4e__NetplayRuntime.hxx"
    "src/sf4e/sf4e__NetplayRuntime.cxx"
    "src/sf4e/sf4e__NetplayRuntime__Input.cxx"
    "src/sf4e/sf4e__NetplayRuntime__Match.cxx"
    "src/sf4e/sf4e__NetplayRuntime__Room.cxx"
    "src/sf4e/sf4e__NetplayRuntime__Tournament.cxx"
    "src/sf4e/sf4e__NetplayRuntime__PublicRooms.cxx"
    "src/sf4e/sf4e__NetplayRuntime__Status.cxx"
    "src/sf4e/sf4e__NetplayPresentation.cxx"
    "src/sf4e/sf4e__RuntimeBridge.hxx"
    "src/sf4e/sf4e__RuntimeBridge.cxx"
)
target_include_directories(sf4e PUBLIC "src/common")
target_link_libraries(sf4e PRIVATE SDL3::SDL3-static dxguid)
option(SF4E_DEVELOPER_UI "Build the fixed developer inspector page" OFF)
if(SF4E_DEVELOPER_UI)
    target_sources(sf4e PRIVATE "src/sf4e/sf4e__DeveloperOverlay.cxx" "src/sf4e/sf4e__DeveloperOverlay__Battle.cxx" "src/sf4e/sf4e__DeveloperOverlay__System.cxx")
    target_compile_definitions(sf4e PRIVATE SF4E_DEVELOPER_UI=1)
endif()
target_link_libraries(sf4e PRIVATE sf4e_steam sf4e_custom_content_files sf4e_replay_files sf4e_video_capture sf4e_netplay_core sf4e_helper_platform sf4e_settings sf4e_ui sf4e_localization)
target_link_libraries(
    sf4e
    PRIVATE ${LIB_DETOURS} Session sf4e_common "Kernel32" "ws2_32" "Mswsock" "AdvApi32" "Shlwapi" "pathcch" "shell32" "Bcrypt" "Xinput9_1_0" "iphlpapi" "Dbghelp" "Winmm"
    PUBLIC imgui::imgui nlohmann_json::nlohmann_json spdlog::spdlog  ggpo::GGPO
)

# The session server core has no game or helper dependency. It is its own
# library so Session, the room host, the server tests and the host benchmark
# all link one build of it, with the flags the shipped binaries use.
add_library(sf4e_session_server STATIC
    "src/session/MatchAuthority.cxx"
    "src/session/sf4e__SessionServer.cxx"
    "src/session/sf4e__SessionServer__Handlers.cxx"
    "src/session/sf4e__SessionServer__Recovery.cxx"
    "src/session/sf4e__SessionServer__RecoveryCheckpoint.cxx"
    "src/session/sf4e__SessionServer__Rooms.cxx"
    "src/session/sf4e__SessionServer__Tournament.cxx"
    "src/session/sf4e__SessionProtocol.cxx"
)
target_compile_features(sf4e_session_server PUBLIC cxx_std_17)
target_compile_definitions(sf4e_session_server PRIVATE NOMINMAX)
target_link_libraries(sf4e_session_server PUBLIC
    nlohmann_json::nlohmann_json spdlog::spdlog sf4e_selection sf4e_room_model sf4e_common "bcrypt")
# The Iroh room over the session server, shared by Session and the room host.
# It talks to the helper and still has no game dependency.
add_library(sf4e_iroh_room STATIC
    "src/session/IrohRoom.cxx"
    "src/session/IrohRoomAdapters.cxx"
    "src/session/IrohRoomCheckpoint.cxx"
    "src/session/IrohRoomPeers.cxx"
    "src/session/IrohRoomPublic.cxx"
    "src/session/IdentityEvents.cxx"
    "src/session/TournamentAnswers.cxx")
target_link_libraries(sf4e_iroh_room PUBLIC sf4e_session_server sf4e_helper_platform)
add_library(Session STATIC
    "src/session/sf4e__SessionClient.hxx"
    "src/session/sf4e__SessionClient.cxx"
    "src/session/sf4e__SessionClient__Room.cxx"
    "src/session/sf4e__SessionProtocol.hxx"
    "src/session/sf4e__SessionServer.hxx"
    "src/session/IrohMatchSession.cxx"
    "src/session/IrohRoom.hxx"
    "src/session/SessionTransport.hxx"
)
target_compile_features(Session PUBLIC cxx_std_17)
target_link_libraries(
    Session
    PRIVATE Dimps sf4e_common sf4e_helper_platform sf4e_room_model
    PUBLIC sf4e_iroh_room nlohmann_json::nlohmann_json spdlog::spdlog  "ws2_32" sf4e_tournament_play
)

# The public room host: the room authority with no game. It links the session
# server and Iroh room libraries and none of the game libraries (Session would
# bring Dimps), so the same sources port to Linux.
add_executable(sf4e-room-host
    "server/roomhost/main.cxx"
    "server/roomhost/RoomHost.cxx"
    "server/roomhost/RoomHostStatus.cxx"
    "server/roomhost/RoomHostHelperWindows.cxx")
target_compile_features(sf4e-room-host PRIVATE cxx_std_17)
target_link_libraries(sf4e-room-host PRIVATE sf4e_iroh_room)

if(BUILD_TESTING)
    sf4e_add_unit_test(NAME SessionClientTransport SOURCES "src/tests/session_client_transport_test.cxx" STD 17 LIBS Session sf4e Dimps "d3d9")
    sf4e_add_unit_test(NAME IrohMatchSession SOURCES "src/tests/iroh_match_session_test.cxx" STD 17 LIBS Session sf4e Dimps "d3d9")
    sf4e_add_unit_test(NAME SpectatorRuntime SOURCES "src/tests/spectator_runtime_test.cxx" STD 17 LIBS Session sf4e sf4e_localization Dimps "d3d9" LOCKS ember_runtime)
    sf4e_add_unit_test(NAME MatchProblem SOURCES "src/tests/match_problem_test.cxx" STD 17 LIBS Session sf4e Dimps "d3d9")
    add_executable(AbortNoticeTest "src/tests/abort_notice_test.cxx")
    target_compile_features(AbortNoticeTest PRIVATE cxx_std_17)
    target_link_libraries(AbortNoticeTest PRIVATE Session sf4e sf4e_localization Dimps "d3d9")
    add_test(NAME AbortNotice COMMAND AbortNoticeTest)
    set_tests_properties(AbortNotice PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "Abort notice passed")
    sf4e_test_locks(AbortNotice loopback_udp_ports)
    add_executable(PresentationSnapshotTest "src/tests/presentation_snapshot_test.cxx")
    target_compile_features(PresentationSnapshotTest PRIVATE cxx_std_17)
    target_link_libraries(PresentationSnapshotTest PRIVATE Session sf4e sf4e_localization Dimps "d3d9")
    add_test(NAME PresentationSnapshot COMMAND PresentationSnapshotTest)
    # Its two-thread check keeps both threads busy for three seconds.
    set_tests_properties(PresentationSnapshot PROPERTIES TIMEOUT 30 PROCESSORS 2 PASS_REGULAR_EXPRESSION "Presentation snapshot passed")
    sf4e_test_locks(PresentationSnapshot ember_runtime)
    if(SF4E_BUILD_IROH_HELPER)
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
        endif()
    endif()
    sf4e_add_unit_test(NAME SessionServerTransport SOURCES "src/tests/session_server_transport_test.cxx" STD 17 LIBS sf4e_iroh_room)
    sf4e_add_unit_test(NAME SessionServerLatency SOURCES "src/tests/session_server_latency_test.cxx" STD 17 LIBS sf4e_iroh_room)
    sf4e_add_unit_test(NAME MemberLatency SOURCES "src/tests/member_latency_test.cxx" STD 17 LIBS sf4e_iroh_room)
    sf4e_add_unit_test(NAME SessionServerModeration SOURCES "src/tests/session_server_moderation_test.cxx" "server/roomhost/RoomHostStatus.cxx"
        LIBS sf4e_session_server)
    sf4e_add_unit_test(NAME RoomHostConfig SOURCES "src/tests/room_host_config_test.cxx" "server/roomhost/RoomHost.cxx" "server/roomhost/RoomHostStatus.cxx"
        STD 17 LIBS sf4e_iroh_room)
    sf4e_add_unit_test(NAME SessionServerReadyTimeout SOURCES "src/tests/session_server_ready_timeout_test.cxx" LIBS sf4e_session_server)
    sf4e_add_unit_test(NAME LowerInputLag SOURCES "src/tests/lower_input_lag_test.cxx" LIBS sf4e_session_server)
    sf4e_add_unit_test(NAME PracticeRoomProjection SOURCES "src/tests/practice_room_projection_test.cxx" LIBS sf4e_session_server)
    sf4e_add_unit_test(NAME DelayPolicyProjection SOURCES "src/tests/delay_policy_projection_test.cxx" LIBS sf4e_session_server)
    sf4e_add_unit_test(NAME SessionServerTournament SOURCES "src/tests/session_server_tournament_test.cxx" LIBS sf4e_session_server)
    # Explicitly invoked host-cost benchmark; not a CTest (see the source).
    add_executable(RoomHostBench "src/tests/room_host_bench.cxx")
    target_link_libraries(RoomHostBench PRIVATE sf4e_session_server)
    sf4e_add_unit_test(NAME GgpoUdpValidation SOURCES "src/tests/ggpo_udp_validation_test.cxx" LIBS ggpo::GGPO "ws2_32" TIMEOUT 15 LOCKS loopback_udp_ports)
    sf4e_add_unit_test(NAME GgpoInputBits SOURCES "src/tests/ggpo_input_bits_test.cxx" TIMEOUT 15)
    add_executable(GgpoSpectatorCapacityTest "src/tests/ggpo_spectator_capacity_test.cxx")
    target_link_libraries(GgpoSpectatorCapacityTest PRIVATE ggpo::GGPO "ws2_32")
    add_test(NAME GgpoSpectatorCapacity COMMAND GgpoSpectatorCapacityTest)
    set_tests_properties(GgpoSpectatorCapacity PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "GGPO spectator capacity passed")
    sf4e_test_locks(GgpoSpectatorCapacity loopback_udp_ports)
    sf4e_add_unit_test(NAME GgpoConfirmedFrame SOURCES "src/tests/ggpo_confirmed_frame_test.cxx" LIBS ggpo::GGPO "ws2_32" TIMEOUT 15 LOCKS loopback_udp_ports)
    sf4e_add_unit_test(NAME GgpoPendingOutput SOURCES "src/tests/ggpo_pending_output_test.cxx" LIBS ggpo::GGPO "ws2_32" TIMEOUT 15 LOCKS loopback_udp_ports)
    sf4e_add_unit_test(NAME GgpoInputGap SOURCES "src/tests/ggpo_input_gap_test.cxx" LIBS ggpo::GGPO "ws2_32" TIMEOUT 15 LOCKS loopback_udp_ports)
    add_executable(GgpoSpectatorHandleControlTest "src/tests/ggpo_spectator_handle_control_test.cxx")
    target_link_libraries(GgpoSpectatorHandleControlTest PRIVATE ggpo::GGPO "ws2_32")
    add_test(NAME GgpoSpectatorHandleControl COMMAND GgpoSpectatorHandleControlTest)
    set_tests_properties(GgpoSpectatorHandleControl PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "GGPO spectator handle control passed")
    sf4e_test_locks(GgpoSpectatorHandleControl loopback_udp_ports)
    sf4e_add_unit_test(NAME GgpoDisconnectBeforeInput SOURCES "src/tests/ggpo_disconnect_before_input_test.cxx" LIBS ggpo::GGPO "ws2_32" TIMEOUT 15 LOCKS loopback_udp_ports)
    sf4e_add_unit_test(NAME GgpoOutageTolerance SOURCES "src/tests/ggpo_outage_tolerance_test.cxx" LIBS ggpo::GGPO "ws2_32" TIMEOUT 45 LOCKS loopback_udp_ports)
    sf4e_add_unit_test(NAME GgpoSyncRetry SOURCES "src/tests/ggpo_sync_retry_test.cxx" LIBS ggpo::GGPO "ws2_32" TIMEOUT 45 LOCKS loopback_udp_ports)
    # Rollback correctness under zero and unequal input delays (docs/validation/ZERO_DELAY_AUDIT.md).
    # The arguments are P1 delay, P2 delay, one-way ms, jitter ms, drop every N, frames, input hold
    # frames and the corrupt-restore control; the plain run is 0/5 under jitter and loss.
    sf4e_add_unit_test(NAME GgpoDelayRollback SOURCES "src/tests/ggpo_delay_rollback_test.cxx" LIBS ggpo::GGPO "ws2_32" "winmm" STD 17 TIMEOUT 50 LOCKS loopback_udp_ports)
    add_test(NAME GgpoDelayRollbackZeroZero COMMAND GgpoDelayRollbackTest 0 0 20 0 0 300 8)
    set_tests_properties(GgpoDelayRollbackZeroZero PROPERTIES TIMEOUT 50)
    sf4e_test_locks(GgpoDelayRollbackZeroZero loopback_udp_ports)
    add_test(NAME GgpoDelayRollbackZeroOne COMMAND GgpoDelayRollbackTest 0 1 40 30 17 300 1)
    set_tests_properties(GgpoDelayRollbackZeroOne PROPERTIES TIMEOUT 50)
    sf4e_test_locks(GgpoDelayRollbackZeroOne loopback_udp_ports)
    add_test(NAME GgpoDelayRollbackFiveZero COMMAND GgpoDelayRollbackTest 5 0 40 30 17 300 1)
    set_tests_properties(GgpoDelayRollbackFiveZero PROPERTIES TIMEOUT 50)
    sf4e_test_locks(GgpoDelayRollbackFiveZero loopback_udp_ports)
    # The control restores a wrong digest and exits 0 only when every frame was confirmed, a restore
    # ran and the digests then disagreed with the oracle.
    add_test(NAME GgpoDelayRollbackCorruptRestore COMMAND GgpoDelayRollbackTest 0 0 25 0 0 180 8 1)
    set_tests_properties(GgpoDelayRollbackCorruptRestore PROPERTIES TIMEOUT 50)
    sf4e_test_locks(GgpoDelayRollbackCorruptRestore loopback_udp_ports)
    add_executable(GgpoSpectatorBacklogTest "src/tests/ggpo_spectator_backlog_test.cxx")
    target_link_libraries(GgpoSpectatorBacklogTest PRIVATE ggpo::GGPO "ws2_32")
    add_test(NAME GgpoSpectatorBacklog COMMAND GgpoSpectatorBacklogTest)
    set_tests_properties(GgpoSpectatorBacklog PROPERTIES TIMEOUT 30 PASS_REGULAR_EXPRESSION "GGPO spectator backlog passed")
    sf4e_test_locks(GgpoSpectatorBacklog loopback_udp_ports)

    sf4e_add_unit_test(NAME RollbackDiagnostics SOURCES "src/tests/rollback_diagnostics_test.cxx" LIBS sf4e_common TIMEOUT 15)
    sf4e_add_unit_test(NAME StateHash SOURCES "src/tests/state_hash_test.cxx" TIMEOUT 15)
    sf4e_add_unit_test(NAME FrameTrace SOURCES "src/tests/frame_trace_test.cxx" STD 14 TIMEOUT 15)
endif()

add_library(sf4e_app_services STATIC src/platform/ApplicationServices.cxx src/platform/UpdatePolicy.cxx src/platform/ReportWorkflow.cxx src/platform/ReportHistory.cxx src/launcher/update/github_release_client.cxx src/launcher/update/github_release_validation.cxx src/launcher/update/github_release_download.cxx)
target_compile_features(sf4e_app_services PUBLIC cxx_std_17)
target_compile_definitions(sf4e_app_services PRIVATE SF4E_APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(sf4e_app_services PUBLIC sf4e_settings sf4e_localization sf4e_replay_files sf4e_problem_report PRIVATE sf4e_common sf4e_selection sf4e_package_installer sf4e_durable_file spdlog::spdlog nlohmann_json::nlohmann_json WIL::WIL pathcch shell32 shlwapi bcrypt ole32)
if(BUILD_TESTING)
    sf4e_add_unit_test(NAME ApplicationServices SOURCES src/tests/application_services_test.cxx LIBS sf4e_app_services)
    sf4e_add_unit_test(NAME UpdatePolicy SOURCES src/tests/update_policy_test.cxx LIBS sf4e_app_services sf4e_settings)
    sf4e_add_unit_test(NAME EmberUpdate SOURCES src/tests/ember_update_test.cxx LIBS sf4e_app_services nlohmann_json::nlohmann_json)
endif()
target_link_libraries(sf4e PRIVATE sf4e_app_services)
add_library(sf4e_recovery STATIC src/ui/RecoverySurface.cxx src/ui/RecoveryController.cxx)
target_compile_features(sf4e_recovery PUBLIC cxx_std_17)
# XInput is loaded at run time; DirectInput 8 is on every supported Windows.
target_link_libraries(sf4e_recovery PUBLIC sf4e_ui sf4e_app_services sf4e_localization PRIVATE d3d9 ole32 shell32 dinput8 dxguid)
if(BUILD_TESTING)
    sf4e_add_unit_test(NAME RecoveryController SOURCES src/tests/recovery_controller_test.cxx LIBS sf4e_recovery TIMEOUT 30)
endif()
add_executable(Launcher WIN32 src/launcher/launcher.cxx src/launcher/netplay/netplay_persist.cxx src/launcher/GameLocator.cxx)
target_link_libraries(Launcher PRIVATE sf4e_helper_platform sf4e_settings sf4e_recovery sf4e_localization sf4e_replay_files sf4e_video_encoder)
# As the video export's encoder (platform/VideoLink.hxx) it holds a second of 4K frames: 4 GB of addresses instead of 2.
target_link_options(Launcher PRIVATE "/LARGEADDRESSAWARE")
set(_SF4E_LAUNCHER_EXE_TARGET Launcher)
target_include_directories(${_SF4E_LAUNCHER_EXE_TARGET} PRIVATE "src/common" "src/launcher")
		target_link_libraries(${_SF4E_LAUNCHER_EXE_TARGET} PRIVATE
    ${LIB_DETOURS}
    CLI11::CLI11
    spdlog::spdlog
    sf4e_common
    nlohmann_json::nlohmann_json
    WIL::WIL
    "shlwapi"
    "pathcch"
    "version"
    "dbghelp"
    "bcrypt"
    "shell32"
    "ole32"
    "oleaut32"
    "user32"
    "gdi32"
    "ws2_32"
)

add_library(Sidecar SHARED src/sidecar/sidecar.cxx)
target_link_libraries(Sidecar PRIVATE ${LIB_DETOURS} Dimps sf4e)
sf4e_configure_windows_version(Sidecar "SF4 Ember Netplay Sidecar" "Sidecar.dll" "VFT_DLL")
sf4e_embed_manifest(Sidecar "Sidecar.dll.manifest")

target_compile_definitions(Launcher PRIVATE -D_UNICODE -DUNICODE "SF4E_APP_VERSION=\"${PROJECT_VERSION}\"")
set_target_properties(Launcher PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
sf4e_configure_windows_version(Launcher "SF4 Ember Netplay" "Launcher.exe" "VFT_APP")
sf4e_embed_manifest(Launcher "Launcher.exe.manifest")
# The source revision the launcher and Sidecar log at startup, refreshed on every build.
set(SF4E_BUILD_IDENTITY_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/identity")
add_custom_target(sf4e_build_identity
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT=${SF4E_BUILD_IDENTITY_DIR}/BuildIdentity.hxx"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sf4e_build_identity.cmake"
    BYPRODUCTS "${SF4E_BUILD_IDENTITY_DIR}/BuildIdentity.hxx"
    VERBATIM)
foreach(target sf4e Launcher sf4e_app_services)
    add_dependencies(${target} sf4e_build_identity)
    target_include_directories(${target} PRIVATE "${SF4E_BUILD_IDENTITY_DIR}")
endforeach()
if(BUILD_TESTING)
    sf4e_add_unit_test(NAME GameLocator SOURCES src/tests/game_locator_test.cxx src/launcher/GameLocator.cxx src/common/SteamKeyValues.cxx STD 17 TIMEOUT 15)
endif()
# A Windows program, not a console one: a console the launcher asks to start
# hidden still opens as an empty terminal where Windows Terminal is the
# default, and the progress window is all the player should see. wmain stays
# its entry point.
add_executable(Updater WIN32
    src/launcher/updater.cxx
)
target_link_options(Updater PRIVATE "/ENTRY:wmainCRTStartup")
# Updater.exe's run under its window, with the process boundaries passed in
# so tests drive it.
add_library(sf4e_updater_run STATIC src/launcher/update/UpdaterRun.cxx)
target_compile_features(sf4e_updater_run PUBLIC cxx_std_17)
target_link_libraries(sf4e_updater_run PUBLIC sf4e_package_installer comctl32 user32)
add_library(sf4e_package_installer STATIC src/launcher/update/PackageInstaller.cxx)
target_compile_features(sf4e_package_installer PUBLIC cxx_std_17)
target_link_libraries(sf4e_package_installer PUBLIC sf4e_selection nlohmann_json::nlohmann_json bcrypt PRIVATE sf4e_durable_file)
if(BUILD_TESTING)
    sf4e_add_unit_test(NAME OverlayPresentation SOURCES src/tests/overlay_presentation_test.cxx STD 17)
    sf4e_add_unit_test(NAME NativeMenuLock SOURCES src/tests/native_menu_lock_test.cxx STD 17)
    sf4e_add_unit_test(NAME OverlayReset SOURCES src/tests/overlay_reset_test.cxx LIBS Session sf4e Dimps sf4e_helper_platform d3d9 TIMEOUT 30 LOCKS ember_runtime)
    sf4e_add_unit_test(NAME OverlayPadCapture SOURCES src/tests/overlay_pad_capture_test.cxx LIBS Session sf4e Dimps sf4e_helper_platform d3d9 TIMEOUT 30 LOCKS ember_runtime)
    sf4e_add_unit_test(NAME OverlayLifecycle SOURCES src/tests/overlay_lifecycle_test.cxx STD 17 TIMEOUT 30)
    # Its stress test keeps three threads busy throughout.
    set_tests_properties(OverlayLifecycle PROPERTIES PROCESSORS 3)
    sf4e_add_unit_test(NAME SoundPoolRestore SOURCES src/tests/sound_pool_restore_test.cxx LIBS Session sf4e Dimps sf4e_helper_platform d3d9 TIMEOUT 15)
    sf4e_add_unit_test(NAME PackageInstaller SOURCES src/tests/package_installer_test.cxx LIBS sf4e_package_installer shell32)
    add_test(NAME UpdaterDownloadValidation COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/scripts/test-updater-download-validation.ps1 -Validator $<TARGET_FILE:PackageInstallerTest>)
    # The record the network fixtures print, produced by the same header, is what
    # the runner's validator is tested against.
    add_executable(NetworkCompletionEmitter src/tests/network_completion_emitter.cxx)
    target_compile_features(NetworkCompletionEmitter PRIVATE cxx_std_17)
    add_test(NAME NetworkCompletion COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/scripts/test-network-completion.ps1 -Emitter $<TARGET_FILE:NetworkCompletionEmitter>)
    sf4e_add_unit_test(NAME ProgressWindow SOURCES src/tests/progress_window_test.cxx LIBS comctl32 user32 STD 17 TIMEOUT 30)
    sf4e_add_unit_test(NAME UpdaterRun SOURCES src/tests/updater_run_test.cxx LIBS sf4e_updater_run STD 17 TIMEOUT 60)
    add_test(NAME UpgradeRecoveryPowerShell COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/scripts/test-upgrade-recovery.ps1 -NativeFixture $<TARGET_FILE:PackageInstallerTest>)
    set_tests_properties(UpgradeRecoveryPowerShell PROPERTIES TIMEOUT 120)
    sf4e_test_locks(UpgradeRecoveryPowerShell ember_processes)
endif()
target_link_libraries(Updater PRIVATE sf4e_updater_run sf4e_selection "pathcch" "shell32")
set_target_properties(Updater PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
sf4e_configure_windows_version(Updater "SF4 Ember Netplay Updater" "Updater.exe" "VFT_APP")

install(
  TARGETS Launcher Sidecar Updater
  RUNTIME DESTINATION "."
)
install(FILES "src/ui/fonts/OFL.txt" DESTINATION "notices" RENAME "Inter-OFL.txt")
install(FILES "src/ui/fonts/NotoSansCJK-OFL.txt" DESTINATION "notices")
install(FILES "src/ui/backends/LICENSE.txt" DESTINATION "notices" RENAME "Dear-ImGui-MIT.txt")
install(FILES "assets/flags/LICENSE" DESTINATION "notices" RENAME "flag-icons-MIT.txt")
# Preserved photographs are masking inputs; the runtime draws their cutouts.
install(DIRECTORY "assets/selection" DESTINATION "assets" COMPONENT SelectionReferences OPTIONAL
    FILES_MATCHING PATTERN "*.png" PATTERN "*.jpg" PATTERN "*.json" PATTERN "*.md"
    REGEX "/color-[0-9]+\\.(png|jpg)$" EXCLUDE)
# Keep local builds self-contained when references have been fetched. Compare
# on each build because image-only changes do not cause Sidecar to relink, and
# write only the files whose content changed.
add_custom_target(SelectionAssets
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/selection" "$<TARGET_FILE_DIR:Sidecar>/assets/selection"
    # The menu art and button prompts the install step ships, so a launch from
    # the build folder does not fall back to text glyphs.
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/brand" "$<TARGET_FILE_DIR:Sidecar>/assets/brand"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/input" "$<TARGET_FILE_DIR:Sidecar>/assets/input"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/flags" "$<TARGET_FILE_DIR:Sidecar>/assets/flags"
    VERBATIM)
add_dependencies(Sidecar SelectionAssets)
add_custom_command(TARGET Sidecar POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Sidecar>/notices"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/fonts/OFL.txt"
        "$<TARGET_FILE_DIR:Sidecar>/notices/Inter-OFL.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/fonts/NotoSansCJK-OFL.txt"
        "$<TARGET_FILE_DIR:Sidecar>/notices/NotoSansCJK-OFL.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/assets/flags/LICENSE"
        "$<TARGET_FILE_DIR:Sidecar>/notices/flag-icons-MIT.txt"
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
