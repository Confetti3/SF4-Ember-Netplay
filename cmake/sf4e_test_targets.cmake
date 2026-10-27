# The tests of the game libraries and the application, registered after every
# library they link exists (CMakeLists.txt includes this file near its end).
# The registration helpers and the labels are in sf4e_tests.cmake.
if(NOT BUILD_TESTING)
    return()
endif()
sf4e_add_unit_test(NAME CheckpointTransfer SOURCES "src/tests/checkpoint_transfer_test.cxx" STD 17 LIBS nlohmann_json::nlohmann_json bcrypt)
sf4e_add_unit_test(NAME RoomLoadWindow SOURCES "src/tests/room_load_window_test.cxx" STD 17 LIBS nlohmann_json::nlohmann_json LABELS room)

# Links Sidecar's own crash record, so the test runs the production handler.
# It needs spdlog and GGPO, so it stays out of the core-only build.
add_executable(HeapCorruptionCaptureTest "src/tests/heap_corruption_capture_test.cxx" "src/sf4e/sf4e__CrashDiagnostics.cxx" "src/sf4e/sf4e__ImportPatch.cxx")
target_compile_features(HeapCorruptionCaptureTest PRIVATE cxx_std_17)
target_link_libraries(HeapCorruptionCaptureTest PRIVATE spdlog::spdlog ggpo::GGPO "dbghelp" "psapi" "shlwapi")
add_test(NAME HeapCorruptionCapture COMMAND HeapCorruptionCaptureTest)
add_test(NAME HeapCheckSettings COMMAND HeapCorruptionCaptureTest heap-check-settings)
add_test(NAME HeapCheckEnvironment COMMAND HeapCorruptionCaptureTest heap-check-environment)
set_tests_properties(HeapCorruptionCapture PROPERTIES TIMEOUT 90)
sf4e_test_labels(LABELS unit TESTS HeapCorruptionCapture HeapCheckSettings HeapCheckEnvironment)

sf4e_add_unit_test(NAME SteamId SOURCES "src/tests/steam_id_test.cxx" TIMEOUT 15)
sf4e_add_unit_test(NAME SteamAdapter SOURCES "src/tests/steam_adapter_test.cxx" LIBS sf4e_steam TIMEOUT 15)

sf4e_add_unit_test(NAME RoomMessageQueue SOURCES "src/tests/room_message_queue_test.cxx" LIBS sf4e_room_model LABELS room)
sf4e_add_unit_test(NAME MatchResultOutbox SOURCES "src/tests/match_result_outbox_test.cxx" LIBS sf4e_netplay_core sf4e_room_model TIMEOUT 15 LABELS room)
sf4e_add_unit_test(NAME ParkedIntent SOURCES "src/tests/parked_intent_test.cxx" TIMEOUT 15)

sf4e_add_unit_test(NAME DiscordPresence SOURCES src/tests/discord_presence_test.cxx LIBS sf4e_room_model LABELS room)
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
sf4e_test_labels(LABELS discord TESTS DiscordBridge)
# Manual live-Discord check; never part of unattended CTest runs.
add_executable(DiscordSmokeTest src/tests/discord_smoke_test.cxx)
target_link_libraries(DiscordSmokeTest PRIVATE sf4e_helper_platform nlohmann_json::nlohmann_json)

sf4e_add_unit_test(NAME RoomAuthority SOURCES "src/tests/room_authority_test.cxx" LIBS sf4e_room_model TIMEOUT 30 LABELS room)
sf4e_add_unit_test(NAME RoomDelayLimits SOURCES "src/tests/room_delay_limits_test.cxx" LIBS sf4e_room_model TIMEOUT 30 LABELS room)
sf4e_add_unit_test(NAME RoomSnapshot SOURCES "src/tests/room_snapshot_test.cxx" LIBS sf4e_room_model TIMEOUT 30 LABELS room)
sf4e_add_unit_test(NAME RoomSteamProfile SOURCES "src/tests/room_steam_profile_test.cxx" LIBS sf4e_room_model TIMEOUT 30 LABELS room)
sf4e_add_unit_test(NAME RoomCountry SOURCES "src/tests/room_country_test.cxx" LIBS sf4e_room_model TIMEOUT 30 LABELS room)
sf4e_add_unit_test(NAME RoomSets SOURCES "src/tests/room_sets_test.cxx" LIBS sf4e_room_model TIMEOUT 30 LABELS room)
sf4e_add_unit_test(NAME RoomTournament SOURCES "src/tests/room_tournament_test.cxx" LIBS sf4e_room_model LABELS room)
sf4e_add_unit_test(NAME TournamentPlay SOURCES "src/tests/tournament_play_test.cxx" LIBS sf4e_tournament_play LABELS room)
sf4e_add_unit_test(NAME IdentityEvents SOURCES "src/tests/identity_events_test.cxx" "src/session/IdentityEvents.cxx"
    LIBS nlohmann_json::nlohmann_json STD 17 LABELS room)
sf4e_add_unit_test(NAME WatchPlay SOURCES "src/tests/watch_play_test.cxx" LIBS sf4e_watch_play LABELS room)
sf4e_add_unit_test(NAME RoomWatch SOURCES "src/tests/room_watch_test.cxx" LIBS sf4e_room_model LABELS room)
sf4e_add_unit_test(NAME RankingRequests SOURCES "src/tests/ranking_requests_test.cxx" STD 17)
sf4e_add_unit_test(NAME TournamentFuzz SOURCES "src/tests/tournament_fuzz_test.cxx" "src/session/TournamentAnswers.cxx"
    LIBS sf4e_tournament_play sf4e_watch_play nlohmann_json::nlohmann_json TIMEOUT 300 LABELS room)
sf4e_add_unit_test(NAME RoomLifecycle SOURCES "src/tests/room_lifecycle_test.cxx" LIBS sf4e_room_model TIMEOUT 30 LABELS room)
sf4e_add_unit_test(NAME RoomServerOwned SOURCES "src/tests/room_server_owned_test.cxx" LIBS sf4e_room_model TIMEOUT 30 LABELS room)
sf4e_add_unit_test(NAME RoomPrivateGolden SOURCES "src/tests/room_private_golden_test.cxx" LIBS sf4e_room_model LABELS room)
sf4e_add_unit_test(NAME RoomSpectatorLock SOURCES "src/tests/room_spectator_lock_test.cxx" LIBS sf4e_room_model TIMEOUT 30 LABELS room)
sf4e_add_unit_test(NAME RoomReadyTimeout SOURCES "src/tests/room_ready_timeout_test.cxx" LIBS sf4e_room_model TIMEOUT 30 LABELS room)
sf4e_add_unit_test(NAME CreatedRules SOURCES "src/tests/created_rules_test.cxx" LIBS sf4e_room_model TIMEOUT 30 LABELS room)
sf4e_add_unit_test(NAME ReadyChime SOURCES "src/tests/ready_chime_test.cxx" LIBS sf4e_room_model TIMEOUT 30 LABELS room)
sf4e_add_unit_test(NAME TrainingCall SOURCES "src/tests/training_call_test.cxx" LIBS sf4e_room_model TIMEOUT 30 LABELS room training)
sf4e_add_unit_test(NAME RoomTrainingFlag SOURCES "src/tests/room_training_flag_test.cxx" LIBS sf4e_room_model TIMEOUT 30 LABELS room)
add_executable(SessionRecoveryGateTest "src/tests/session_recovery_gate_test.cxx")
target_compile_features(SessionRecoveryGateTest PRIVATE cxx_std_14)
target_link_libraries(SessionRecoveryGateTest PRIVATE sf4e_room_model)
if(MSVC)
    target_compile_options(SessionRecoveryGateTest PRIVATE /UNDEBUG)
else()
    target_compile_options(SessionRecoveryGateTest PRIVATE -UNDEBUG)
endif()
add_test(NAME SessionRecoveryGate COMMAND SessionRecoveryGateTest)
sf4e_add_unit_test(NAME CheckpointDecodeWorker SOURCES "src/tests/checkpoint_decode_worker_test.cxx" STD 14 LIBS sf4e_room_model TIMEOUT 30 LABELS room)
sf4e_add_unit_test(NAME SpectatorPolicy SOURCES "src/tests/spectator_policy_test.cxx" LABELS room)
sf4e_add_unit_test(NAME NextBattleRequest SOURCES "src/tests/next_battle_request_test.cxx" STD 14 TIMEOUT 15)
sf4e_add_unit_test(NAME SpectatorSync SOURCES "src/tests/spectator_sync_test.cxx" STD 14 LIBS sf4e_room_model TIMEOUT 15 LABELS room)
sf4e_add_unit_test(NAME GameDisplayConfig SOURCES "src/tests/game_display_config_test.cxx" STD 17)
sf4e_add_unit_test(NAME FrameRateWindow SOURCES "src/tests/frame_rate_window_test.cxx" STD 17)

sf4e_add_unit_test(NAME SelectionValue SOURCES "src/tests/selection_value_test.cxx" LIBS nlohmann_json::nlohmann_json TIMEOUT 15)
sf4e_add_unit_test(NAME MoveNotation SOURCES "src/tests/move_notation_test.cxx" STD 14 LIBS nlohmann_json::nlohmann_json TIMEOUT 15)
sf4e_add_unit_test(NAME HitBoxes SOURCES "src/tests/hit_boxes_test.cxx" STD 14 TIMEOUT 15)
sf4e_add_unit_test(NAME BacFile SOURCES "src/tests/bac_file_test.cxx" STD 14 TIMEOUT 15)
sf4e_add_unit_test(NAME PracticeSettings SOURCES "src/tests/practice_settings_test.cxx" STD 14 LIBS nlohmann_json::nlohmann_json TIMEOUT 15)
sf4e_add_unit_test(NAME TrainingDummyControl SOURCES "src/tests/training_dummy_control_test.cxx" STD 14 LIBS nlohmann_json::nlohmann_json LABELS training)

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

# It takes 2 s alone. Its saves are flushed writes, which take up to ten
# times as long while other tests and builds use the disk.
sf4e_add_unit_test(NAME SettingsStore SOURCES "src/tests/settings_store_test.cxx" LIBS sf4e_settings sf4e_durable_file TIMEOUT 60)
sf4e_add_unit_test(NAME UiPreferencesStore SOURCES "src/tests/ui_preferences_store_test.cxx" LIBS sf4e_settings)
sf4e_add_unit_test(NAME RecordingFileStore SOURCES "src/tests/recording_file_store_test.cxx" LIBS sf4e_settings sf4e_durable_file TIMEOUT 60)

sf4e_add_unit_test(NAME ProblemReport SOURCES "src/tests/problem_report_test.cxx" LIBS sf4e_problem_report TIMEOUT 30)

# The scenario fragments' non-template parts compile on their own; the
# scenarios still run in the order ui_render_test.cxx calls them.
add_executable(UiRenderTest "src/tests/ui_render_test.cxx" "src/tests/ui_render_match_hud.cxx" "src/tests/ui_render_overlay.cxx")
target_compile_definitions(UiRenderTest PRIVATE SF4E_TEST_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets/selection")
sf4e_add_unit_test(NAME ControllerNavigation SOURCES src/tests/controller_navigation_test.cxx LIBS sf4e_ui Dimps TIMEOUT 30 LABELS ui)
sf4e_add_unit_test(NAME MenuButtons SOURCES src/tests/menu_buttons_test.cxx LIBS sf4e_ui Dimps sf4e_settings STD 17 LABELS ui)
sf4e_add_unit_test(NAME GamepadPrompts SOURCES src/tests/gamepad_prompts_test.cxx LIBS sf4e_ui STD 17 LABELS ui)
sf4e_add_unit_test(NAME ButtonLayout SOURCES src/tests/button_layout_test.cxx LIBS sf4e_settings STD 17)
sf4e_add_unit_test(NAME PadSetup SOURCES src/tests/pad_setup_test.cxx STD 17)
sf4e_add_unit_test(NAME PadSetupMapping SOURCES src/tests/pad_setup_mapping_test.cxx LIBS SDL3::SDL3-static sf4e_settings STD 17)
sf4e_add_unit_test(NAME GamepadSlots SOURCES src/tests/gamepad_slots_test.cxx STD 17)
sf4e_add_unit_test(NAME GamepadRumble SOURCES src/tests/gamepad_rumble_test.cxx STD 17)
sf4e_add_unit_test(NAME ControllersScreen SOURCES src/tests/controllers_screen_test.cxx LIBS sf4e_ui STD 17 LABELS ui)
sf4e_add_unit_test(NAME GamepadSlotsImportPatch SOURCES src/tests/import_patch_test.cxx src/sf4e/sf4e__ImportPatch.cxx STD 17)
sf4e_add_unit_test(NAME DirectInputFilter SOURCES src/tests/directinput_filter_test.cxx LIBS sf4e dxguid STD 17)
sf4e_add_unit_test(NAME ShellJourney SOURCES src/tests/shell_journey_test.cxx src/tests/shell_replay_journey_test.cxx
    src/tests/shell_room_journey_test.cxx src/tests/shell_selection_journey_test.cxx
    src/tests/shell_chat_journey_test.cxx src/tests/shell_training_journey_test.cxx src/tests/shell_prompt_journey_test.cxx src/tests/shell_controller_journey_test.cxx src/tests/shell_button_layout_journey_test.cxx
    src/tests/shell_feedback_journey_test.cxx src/tests/shell_selector_journey_test.cxx src/tests/shell_update_journey_test.cxx
    LIBS sf4e_ui Dimps TIMEOUT 30 LABELS ui)
sf4e_add_unit_test(NAME IdentityJourney SOURCES src/tests/identity_journey_test.cxx src/tests/discord_connect_journey_test.cxx
    src/tests/builtin_service_journey_test.cxx src/tests/sign_in_site_journey_test.cxx
    LIBS sf4e_ui Dimps TIMEOUT 30 LABELS ui)
sf4e_add_unit_test(NAME WatchJourney SOURCES src/tests/watch_journey_test.cxx LIBS sf4e_ui Dimps TIMEOUT 30 LABELS ui room)
sf4e_add_unit_test(NAME PublicRoomsJourney SOURCES src/tests/public_rooms_journey_test.cxx src/tests/public_rooms_setup_journey_test.cxx
    LIBS sf4e_ui Dimps TIMEOUT 30 LABELS ui room)
sf4e_add_unit_test(NAME RankingsJourney SOURCES src/tests/rankings_journey_test.cxx LIBS sf4e_ui Dimps TIMEOUT 30 LABELS ui)
sf4e_add_unit_test(NAME RoomPanelNavigation SOURCES src/tests/room_panel_navigation_test.cxx LIBS sf4e_ui Dimps TIMEOUT 30 LABELS ui room)
sf4e_add_unit_test(NAME WatchedMatchExit SOURCES src/tests/watched_match_exit_test.cxx LIBS sf4e_ui Dimps TIMEOUT 30 LABELS ui room)
sf4e_add_unit_test(NAME LatencyMark SOURCES src/tests/latency_mark_test.cxx LIBS sf4e_ui Dimps TIMEOUT 15 LABELS ui)
sf4e_add_unit_test(NAME ReplayScreenText SOURCES src/tests/replay_screen_text_test.cxx LIBS sf4e_ui Dimps TIMEOUT 15 LABELS ui replay)
sf4e_add_unit_test(NAME NetworkFeedback SOURCES src/tests/network_feedback_test.cxx LIBS sf4e_ui Dimps TIMEOUT 30 LABELS ui)
sf4e_add_unit_test(NAME NativeSelectionAvailability SOURCES src/tests/native_selection_availability_test.cxx LIBS sf4e_ui Dimps TIMEOUT 30 LABELS ui)
sf4e_add_unit_test(NAME WineBuiltin SOURCES src/tests/wine_builtin_test.cxx STD 17)
sf4e_add_unit_test(NAME Win32Cursor SOURCES src/tests/win32_cursor_test.cxx LIBS sf4e_ui LABELS ui)
sf4e_add_unit_test(NAME CountryPicker SOURCES src/tests/country_picker_test.cxx LIBS sf4e_ui TIMEOUT 30 LABELS ui)
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
# Every fighter's costume and color galleries with the packaged previews,
# English at one size. No game folder: a portrait falls back to the
# original costume's first color, and the game's Random stage tile is not
# checked. A folder in place of "-" keeps each page.
add_test(NAME UiRenderGallery COMMAND UiRenderTest - "${CMAKE_BINARY_DIR}/no-game" "${CMAKE_SOURCE_DIR}/assets/selection")
set_tests_properties(UiRenderGallery PROPERTIES TIMEOUT 600 ENVIRONMENT "SF4E_UI_RENDER_GALLERY=1;SF4E_UI_RENDER_QUICK=1")
add_executable(SelectionArtTest src/tests/selection_art_test.cxx)
target_compile_definitions(SelectionArtTest PRIVATE SF4E_TEST_ASSET_ROOT="${CMAKE_SOURCE_DIR}/assets/selection")
target_link_libraries(SelectionArtTest PRIVATE sf4e_ui "d3d9")
add_test(NAME SelectionArt COMMAND SelectionArtTest)
set_tests_properties(SelectionArt PROPERTIES TIMEOUT 60)

sf4e_add_unit_test(NAME NetworkLink SOURCES "src/tests/network_link_test.cxx" LIBS sf4e_common TIMEOUT 30)
sf4e_add_unit_test(NAME HttpPostCancel SOURCES "src/tests/http_post_cancel_test.cxx" STD 17 LIBS sf4e_common ws2_32 TIMEOUT 60)

sf4e_add_unit_test(NAME SessionClientTransport SOURCES "src/tests/session_client_transport_test.cxx" STD 17 LIBS Session sf4e Dimps "d3d9" LABELS room)
sf4e_add_unit_test(NAME IrohMatchSession SOURCES "src/tests/iroh_match_session_test.cxx" STD 17 LIBS Session sf4e Dimps "d3d9" LABELS room)
sf4e_add_unit_test(NAME SpectatorRuntime SOURCES "src/tests/spectator_runtime_test.cxx" STD 17 LIBS Session sf4e sf4e_localization Dimps "d3d9" LOCKS ember_runtime LABELS room)
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

sf4e_add_unit_test(NAME SessionServerTransportLobby SOURCES "src/tests/session_server_transport_lobby_test.cxx" STD 17 LIBS sf4e_iroh_room LABELS room)
sf4e_add_unit_test(NAME SessionServerTransportBarriers SOURCES "src/tests/session_server_transport_barriers_test.cxx" STD 17 LIBS sf4e_iroh_room LABELS room)
sf4e_add_unit_test(NAME SessionServerTransportTerminal SOURCES "src/tests/session_server_transport_terminal_test.cxx" STD 17 LIBS sf4e_iroh_room LABELS room)
sf4e_add_unit_test(NAME SessionServerTransportCommit SOURCES "src/tests/session_server_transport_commit_test.cxx" STD 17 LIBS sf4e_iroh_room LABELS room)
sf4e_add_unit_test(NAME SessionServerTransportDepartures SOURCES "src/tests/session_server_transport_departures_test.cxx" STD 17 LIBS sf4e_iroh_room LABELS room)
sf4e_add_unit_test(NAME SessionServerLatency SOURCES "src/tests/session_server_latency_test.cxx" STD 17 LIBS sf4e_iroh_room LABELS room)
sf4e_add_unit_test(NAME MemberLatency SOURCES "src/tests/member_latency_test.cxx" STD 17 LIBS sf4e_iroh_room LABELS room)
sf4e_add_unit_test(NAME SessionServerModeration SOURCES "src/tests/session_server_moderation_test.cxx" "server/roomhost/RoomHostStatus.cxx"
    LIBS sf4e_session_server LABELS room)
sf4e_add_unit_test(NAME RoomHostConfig SOURCES "src/tests/room_host_config_test.cxx" "server/roomhost/RoomHost.cxx" "server/roomhost/RoomHostStatus.cxx"
    STD 17 LIBS sf4e_iroh_room LABELS room)
sf4e_add_unit_test(NAME SessionServerReadyTimeout SOURCES "src/tests/session_server_ready_timeout_test.cxx" LIBS sf4e_session_server LABELS room)
sf4e_add_unit_test(NAME LowerInputLag SOURCES "src/tests/lower_input_lag_test.cxx" LIBS sf4e_session_server LABELS room)
sf4e_add_unit_test(NAME PracticeRoomProjection SOURCES "src/tests/practice_room_projection_test.cxx" LIBS sf4e_session_server LABELS room)
sf4e_add_unit_test(NAME DelayPolicyProjection SOURCES "src/tests/delay_policy_projection_test.cxx" LIBS sf4e_session_server LABELS room)
sf4e_add_unit_test(NAME SessionServerTournament SOURCES "src/tests/session_server_tournament_test.cxx" LIBS sf4e_session_server LABELS room)
sf4e_add_unit_test(NAME SessionServerWatch SOURCES "src/tests/session_server_watch_test.cxx" LIBS sf4e_session_server ZLIB::ZLIB LABELS room)
# The fixture the helper's own test parses too (rust/sf4-net, service/tests/watch.rs).
target_compile_definitions(SessionServerWatchTest PRIVATE SF4E_TEST_FIXTURES="${CMAKE_SOURCE_DIR}/src/tests/fixtures")
# Explicitly invoked host-cost benchmark; not a CTest (see the source).
add_executable(RoomHostBench "src/tests/room_host_bench.cxx")
target_link_libraries(RoomHostBench PRIVATE sf4e_session_server)
sf4e_add_unit_test(NAME GgpoUdpValidation SOURCES "src/tests/ggpo_udp_validation_test.cxx" LIBS ggpo::GGPO "ws2_32" TIMEOUT 15 LOCKS loopback_udp_ports LABELS ggpo)
sf4e_add_unit_test(NAME GgpoInputBits SOURCES "src/tests/ggpo_input_bits_test.cxx" TIMEOUT 15 LABELS ggpo)
add_executable(GgpoSpectatorCapacityTest "src/tests/ggpo_spectator_capacity_test.cxx")
target_link_libraries(GgpoSpectatorCapacityTest PRIVATE ggpo::GGPO "ws2_32")
add_test(NAME GgpoSpectatorCapacity COMMAND GgpoSpectatorCapacityTest)
set_tests_properties(GgpoSpectatorCapacity PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "GGPO spectator capacity passed")
sf4e_test_locks(GgpoSpectatorCapacity loopback_udp_ports)
sf4e_add_unit_test(NAME GgpoConfirmedFrame SOURCES "src/tests/ggpo_confirmed_frame_test.cxx" LIBS ggpo::GGPO "ws2_32" TIMEOUT 15 LOCKS loopback_udp_ports LABELS ggpo)
sf4e_add_unit_test(NAME GgpoPendingOutput SOURCES "src/tests/ggpo_pending_output_test.cxx" LIBS ggpo::GGPO "ws2_32" TIMEOUT 15 LOCKS loopback_udp_ports LABELS ggpo)
sf4e_add_unit_test(NAME GgpoInputGap SOURCES "src/tests/ggpo_input_gap_test.cxx" LIBS ggpo::GGPO "ws2_32" TIMEOUT 15 LOCKS loopback_udp_ports LABELS ggpo)
add_executable(GgpoSpectatorHandleControlTest "src/tests/ggpo_spectator_handle_control_test.cxx")
target_link_libraries(GgpoSpectatorHandleControlTest PRIVATE ggpo::GGPO "ws2_32")
add_test(NAME GgpoSpectatorHandleControl COMMAND GgpoSpectatorHandleControlTest)
set_tests_properties(GgpoSpectatorHandleControl PROPERTIES TIMEOUT 15 PASS_REGULAR_EXPRESSION "GGPO spectator handle control passed")
sf4e_test_locks(GgpoSpectatorHandleControl loopback_udp_ports)
sf4e_add_unit_test(NAME GgpoDisconnectBeforeInput SOURCES "src/tests/ggpo_disconnect_before_input_test.cxx" LIBS ggpo::GGPO "ws2_32" TIMEOUT 15 LOCKS loopback_udp_ports LABELS ggpo)
sf4e_add_unit_test(NAME GgpoOutageTolerance SOURCES "src/tests/ggpo_outage_tolerance_test.cxx" LIBS ggpo::GGPO "ws2_32" TIMEOUT 45 LOCKS loopback_udp_ports LABELS ggpo)
sf4e_add_unit_test(NAME GgpoSyncRetry SOURCES "src/tests/ggpo_sync_retry_test.cxx" LIBS ggpo::GGPO "ws2_32" TIMEOUT 45 LOCKS loopback_udp_ports LABELS ggpo)
# Rollback correctness under zero and unequal input delays (docs/validation/ZERO_DELAY_AUDIT.md).
# The arguments are P1 delay, P2 delay, one-way ms, jitter ms, drop every N, frames, input hold
# frames and the corrupt-restore control; the plain run is 0/5 under jitter and loss.
sf4e_add_unit_test(NAME GgpoDelayRollback SOURCES "src/tests/ggpo_delay_rollback_test.cxx" LIBS ggpo::GGPO "ws2_32" "winmm" STD 17 TIMEOUT 50 LOCKS loopback_udp_ports LABELS ggpo)
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

sf4e_add_unit_test(NAME ApplicationServices SOURCES src/tests/application_services_test.cxx LIBS sf4e_app_services)
sf4e_add_unit_test(NAME UpdatePolicy SOURCES src/tests/update_policy_test.cxx LIBS sf4e_app_services sf4e_settings)
sf4e_add_unit_test(NAME EmberUpdate SOURCES src/tests/ember_update_test.cxx LIBS sf4e_app_services nlohmann_json::nlohmann_json ZLIB::ZLIB)

sf4e_add_unit_test(NAME RecoveryController SOURCES src/tests/recovery_controller_test.cxx LIBS sf4e_recovery TIMEOUT 30 LABELS ui)

sf4e_add_unit_test(NAME GameLocator SOURCES src/tests/game_locator_test.cxx src/launcher/GameLocator.cxx src/common/SteamKeyValues.cxx STD 17 TIMEOUT 15)

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

# The tests registered above with add_test, which carry no labels from a helper.
sf4e_test_labels(LABELS unit TESTS Localization Countries AbortNotice PresentationSnapshot
    GgpoSpectatorCapacity GgpoSpectatorHandleControl GgpoSpectatorBacklog GgpoDelayRollbackZeroZero
    GgpoDelayRollbackZeroOne GgpoDelayRollbackFiveZero GgpoDelayRollbackCorruptRestore SessionRecoveryGate)
sf4e_test_labels(LABELS ggpo TESTS GgpoSpectatorCapacity GgpoSpectatorHandleControl GgpoSpectatorBacklog
    GgpoDelayRollbackZeroZero GgpoDelayRollbackZeroOne GgpoDelayRollbackFiveZero GgpoDelayRollbackCorruptRestore)
sf4e_test_labels(LABELS room TESTS SessionRecoveryGate)
sf4e_test_labels(LABELS ui TESTS UiRender UiRenderLocales UiRenderAtlas UiRenderGallery SelectionArt)
sf4e_test_labels(LABELS script TESTS UpdaterDownloadValidation NetworkCompletion UpgradeRecoveryPowerShell)

# The publisher's PowerShell suites. They mock git, gh, ssh, scp and WSL and write only under TEMP, so
# nothing here builds or publishes. A receipt with testsPassed proves they ran: Invoke-DefaultTests in
# scripts/BuildProvenance.ps1 refuses a run without them, and the fake CTest in
# scripts/test-build-provenance.ps1 names them. The Nightly scripts need PowerShell 7.
find_program(SF4E_PWSH pwsh REQUIRED)
# Git for Windows' bash.exe for the Bash checks of NightlyPaths: never a WSL launcher that may be first on
# PATH, so it is looked for beside git as Get-EmberGitBash does (scripts/BuildEnvironment.ps1). Set
# SF4E_GIT_BASH to use another; the configure fails without one, as the suite does not skip its Bash checks.
if(NOT SF4E_GIT_BASH)
    if(DEFINED ENV{SF4E_GIT_BASH})
        set(_sf4e_bash "$ENV{SF4E_GIT_BASH}")
    else()
        find_program(SF4E_GIT_EXECUTABLE git)
        set(_sf4e_bash "")
        if(SF4E_GIT_EXECUTABLE)
            get_filename_component(_sf4e_dir "${SF4E_GIT_EXECUTABLE}" DIRECTORY)
            foreach(_level RANGE 3)
                if(EXISTS "${_sf4e_dir}/bin/bash.exe")
                    set(_sf4e_bash "${_sf4e_dir}/bin/bash.exe")
                    break()
                endif()
                get_filename_component(_sf4e_dir "${_sf4e_dir}" DIRECTORY)
            endforeach()
        endif()
    endif()
    if(NOT _sf4e_bash OR NOT EXISTS "${_sf4e_bash}")
        message(FATAL_ERROR "NightlyPaths needs Git for Windows' bash.exe: install Git for Windows or set SF4E_GIT_BASH")
    endif()
    set(SF4E_GIT_BASH "${_sf4e_bash}" CACHE FILEPATH "Git for Windows' bash.exe, for the NightlyPaths test" FORCE)
endif()
foreach(suite "BuildProvenance;test-build-provenance.ps1;300" "NightlyPublication;test-nightly-publication.ps1;600"
        "PackageFiles;test-package-files.ps1;300")
    list(GET suite 0 suite_name)
    list(GET suite 1 suite_script)
    list(GET suite 2 suite_timeout)
    add_test(NAME ${suite_name} COMMAND "${SF4E_PWSH}" -NoProfile -File "scripts/${suite_script}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(${suite_name} PROPERTIES TIMEOUT ${suite_timeout})
endforeach()
add_test(NAME NightlyPaths COMMAND "${SF4E_PWSH}" -NoProfile -File scripts/test-nightly-paths.ps1 -BashPath "${SF4E_GIT_BASH}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(NightlyPaths PROPERTIES TIMEOUT 300)
sf4e_test_labels(LABELS publisher script TESTS BuildProvenance NightlyPublication NightlyPaths PackageFiles)
