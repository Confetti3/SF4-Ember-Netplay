cmake_minimum_required (VERSION 3.26)

include("${CMAKE_SOURCE_DIR}/cmake/sf4e_version.cmake")

project (
    sf4e
    VERSION 1.5.0
    DESCRIPTION "SF4 Ember Netplay: private rollback rooms and an in-game overlay for Ultra Street Fighter IV."
    HOMEPAGE_URL "https://github.com/Confetti3/SF4-Ember-Netplay"
)

include(CTest)
include("${CMAKE_SOURCE_DIR}/cmake/sf4e_tests.cmake")
# The room host's C++ sources, one list shared with server/roomhost/build-linux.sh.
include("${CMAKE_SOURCE_DIR}/cmake/sf4e_roomhost_sources.cmake")

option(SF4E_CORE_TESTS_ONLY "Build game-independent migration tests without Windows/game dependencies" OFF)
add_library(sf4e_netplay_core STATIC "src/netplay/SessionController.cxx" "src/netplay/MatchResultOutbox.cxx")
add_library(sf4e_selection STATIC ${SF4E_ROOMHOST_selection} "src/common/InstalledContent.cxx")
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
include("${CMAKE_SOURCE_DIR}/cmake/sf4e_core_tests.cmake")
if(SF4E_CORE_TESTS_ONLY)
    sf4e_check_test_labels()
    return()
endif()
# The in-game self-test (docs/development/IN_GAME_SELFTEST.md), in the game
# and in the helper it starts.
option(SF4E_SELFTEST "Build the in-game self-test" OFF)
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
include("${CMAKE_SOURCE_DIR}/cmake/sf4e_discord.cmake")
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(wil CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(SDL3 CONFIG REQUIRED)

add_library(sf4e_room_model STATIC ${SF4E_ROOMHOST_room_model})
target_compile_features(sf4e_room_model PUBLIC cxx_std_14)
target_link_libraries(sf4e_room_model PUBLIC nlohmann_json::nlohmann_json "bcrypt")
# The game's Steam client through steam_api.dll (profile overlay, remote storage).
add_library(sf4e_steam STATIC "src/sf4e/sf4e__Steam.cxx")
target_compile_features(sf4e_steam PUBLIC cxx_std_11)
target_link_libraries(sf4e_steam PUBLIC spdlog::spdlog)
# Playing a bridge-run tournament match: a pure state machine the runtime ticks.
add_library(sf4e_tournament_play STATIC "src/netplay/TournamentPlay.cxx")
target_compile_features(sf4e_tournament_play PUBLIC cxx_std_17)
target_link_libraries(sf4e_tournament_play PUBLIC sf4e_room_model)
# Watching one: the same, a pure state machine the runtime ticks beside it. It
# reads the room through TournamentPlay's RoomView.
add_library(sf4e_watch_play STATIC "src/netplay/WatchPlay.cxx")
target_compile_features(sf4e_watch_play PUBLIC cxx_std_17)
target_link_libraries(sf4e_watch_play PUBLIC sf4e_tournament_play)


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

# The UI preference store is a settings store that validates one localization
# value, so it lives with the settings and depends on localization, not both
# ways around.
add_library(sf4e_settings STATIC "src/netplay/JsonFileStore.cxx" "src/netplay/SettingsStore.cxx"
    "src/netplay/SettingsWriter.cxx" "src/platform/UiPreferencesStore.cxx"
    "src/platform/GameDisplaySettings.cxx")
target_compile_features(sf4e_settings PUBLIC cxx_std_17)
target_link_libraries(sf4e_settings PUBLIC nlohmann_json::nlohmann_json sf4e_localization PRIVATE sf4e_durable_file "shell32" "ole32")

add_library(sf4e_ui STATIC
    src/ui/ApplicationShell.cxx src/ui/ControllersScreen.cxx src/ui/ReplaysPanel.cxx src/ui/RoomPanel.cxx src/ui/RoomActions.cxx src/ui/RoomControls.cxx
    src/ui/Theme.cxx src/ui/MatchHud.cxx src/ui/SelectionArt.cxx src/ui/FighterSelector.cxx
    src/ui/Win32Input.cxx src/ui/Win32InputBridge.cxx
    src/ui/GameMenu.cxx src/ui/MenuDialogs.cxx src/ui/ChatTranscript.cxx src/ui/ChatPanel.cxx
    src/ui/ControllerNavigation.cxx src/ui/backends/imgui_impl_win32.cpp
    src/ui/TrainingPanel.cxx src/ui/TrainingMeter.cxx src/ui/TrainingShortcuts.cxx src/ui/IdentityPanel.cxx src/ui/IdentityPanel__Watch.cxx src/ui/IdentityConnect.cxx src/ui/IdentitySignIn.cxx src/ui/IdentityMatches.cxx src/ui/IdentityDesk.cxx src/ui/PublicRoomsPanel.cxx src/ui/PublicRoomsCards.cxx src/ui/PublicRoomsSetup.cxx src/ui/RankingsPanel.cxx src/ui/RankingsCards.cxx src/ui/MatchAnnouncements.cxx
    src/ui/OverlayLayers.cxx src/ui/ReplayPlaybackHud.cxx src/ui/TrainingInRoom.cxx src/ui/ApplicationShell__Screens.cxx
    src/ui/ProblemReportPanel.cxx src/ui/PadSetupScreen.cxx src/ui/ApplicationShell__PadSetup.cxx src/ui/ButtonLayoutScreen.cxx src/ui/ApplicationShell__ButtonLayout.cxx src/ui/ApplicationShell__Create.cxx src/ui/ApplicationShell__Links.cxx src/ui/RoomCards.cxx src/ui/ApplicationShell__Updates.cxx src/ui/UpdatePrompt.cxx src/ui/CountryFlag.cxx src/ui/CountryPicker.cxx src/ui/NameMarks.cxx)
install(DIRECTORY "assets/brand" DESTINATION "assets" FILES_MATCHING PATTERN "*.png")
install(DIRECTORY "assets/input" DESTINATION "assets" FILES_MATCHING PATTERN "*.png" PATTERN "*.txt")
install(DIRECTORY "assets/flags" DESTINATION "assets" FILES_MATCHING PATTERN "flags.png" PATTERN "flags.json" PATTERN "LICENSE" PATTERN "README.md")
set_source_files_properties(src/ui/backends/imgui_impl_win32.cpp PROPERTIES COMPILE_DEFINITIONS IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
target_compile_features(sf4e_ui PUBLIC cxx_std_17)
add_library(sf4e_problem_report STATIC src/platform/ProblemReport.cxx)
target_compile_features(sf4e_problem_report PUBLIC cxx_std_17)
target_link_libraries(sf4e_problem_report PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(sf4e_ui PUBLIC sf4e_problem_report)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sf4e_fonts.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sf4e_brand.cmake")
target_link_libraries(sf4e_ui PUBLIC imgui::imgui sf4e_selection sf4e_room_model sf4e_localization sf4e_settings PRIVATE ZLIB::ZLIB "windowscodecs" "ole32" "d3d9")
target_compile_definitions(sf4e_ui PRIVATE SF4E_BUILD_VERSION="${PROJECT_VERSION}")

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
    ${SF4E_ROOMHOST_common}
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
    "src/sf4e/sf4e__Game__Battle__System__GgpoCallbacks.cxx"
    "src/sf4e/sf4e__Game__Battle__System__GgpoPacing.cxx"
    "src/sf4e/sf4e__Game__Battle__System__Memento.cxx"
    "src/sf4e/sf4e__Game__Battle__System__NativeResult.cxx"
    "src/sf4e/sf4e__Game__Battle__System__RollbackStress.cxx"
    "src/sf4e/sf4e__Game__Battle__System__SaveState.cxx"
    "src/sf4e/sf4e__Game__Battle__System__SaveStateRelease.cxx"
    "src/sf4e/sf4e__Game__Battle__System__StateHash.cxx"
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
    "src/sf4e/sf4e__MenuMusic.hxx"
    "src/sf4e/sf4e__MenuMusic.cxx"
    "src/sf4e/sf4e__Borderless.hxx"
    "src/sf4e/sf4e__Borderless.cxx"
    "src/sf4e/sf4e__StartupLog.hxx"
    "src/sf4e/sf4e__StartupLog.cxx"
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
    "src/sf4e/sf4e__NetplayRuntime__Rankings.cxx"
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
# The in-game self-test (docs/development/IN_GAME_SELFTEST.md). Without it
# selftest::Running is false and nothing of the test is built.
if(SF4E_SELFTEST)
    target_sources(sf4e PRIVATE "src/sf4e/sf4e__SelfTest.cxx" "src/sf4e/sf4e__SelfTest__Training.cxx" "src/sf4e/sf4e__SelfTest__Room.cxx" "src/sf4e/sf4e__SelfTest__RoomLayout.cxx" "src/sf4e/sf4e__SelfTest__Replay.cxx" "src/sf4e/sf4e__SelfTest__Online.cxx" "src/sf4e/sf4e__SelfTest__Tournament.cxx")
    target_compile_definitions(sf4e PRIVATE SF4E_SELFTEST=1)
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
add_library(sf4e_session_server STATIC ${SF4E_ROOMHOST_session_server})
target_compile_features(sf4e_session_server PUBLIC cxx_std_17)
target_compile_definitions(sf4e_session_server PRIVATE NOMINMAX)
target_link_libraries(sf4e_session_server PUBLIC
    nlohmann_json::nlohmann_json spdlog::spdlog sf4e_selection sf4e_room_model sf4e_common "bcrypt")
# The Iroh room over the session server, shared by Session and the room host.
# It talks to the helper and still has no game dependency.
add_library(sf4e_iroh_room STATIC ${SF4E_ROOMHOST_iroh_room})
target_link_libraries(sf4e_iroh_room PUBLIC sf4e_session_server sf4e_helper_platform)
add_library(Session STATIC
    "src/session/sf4e__SessionClient.hxx"
    "src/session/sf4e__SessionClient.cxx"
    "src/session/sf4e__SessionClient__Room.cxx"
    "src/session/sf4e__SessionClient__Verification.cxx"
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
    PUBLIC sf4e_iroh_room nlohmann_json::nlohmann_json spdlog::spdlog  "ws2_32" sf4e_tournament_play sf4e_watch_play
)

# The public room host: the room authority with no game. It links the session
# server and Iroh room libraries and none of the game libraries (Session would
# bring Dimps), so the same sources port to Linux.
add_executable(sf4e-room-host ${SF4E_ROOMHOST_host} ${SF4E_ROOMHOST_host_windows})
target_compile_features(sf4e-room-host PRIVATE cxx_std_17)
target_link_libraries(sf4e-room-host PRIVATE sf4e_iroh_room)


add_library(sf4e_app_services STATIC src/platform/ApplicationServices.cxx src/platform/UpdatePolicy.cxx src/platform/ReportWorkflow.cxx src/platform/ReportHistory.cxx src/launcher/update/github_release_client.cxx src/launcher/update/github_release_validation.cxx src/launcher/update/github_release_download.cxx)
target_compile_features(sf4e_app_services PUBLIC cxx_std_17)
target_compile_definitions(sf4e_app_services PRIVATE SF4E_APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(sf4e_app_services PUBLIC sf4e_settings sf4e_localization sf4e_replay_files sf4e_problem_report PRIVATE sf4e_common sf4e_selection sf4e_package_installer sf4e_durable_file spdlog::spdlog nlohmann_json::nlohmann_json WIL::WIL pathcch shell32 shlwapi bcrypt ole32)
target_link_libraries(sf4e PRIVATE sf4e_app_services)
add_library(sf4e_recovery STATIC src/ui/RecoverySurface.cxx src/ui/RecoveryController.cxx)
target_compile_features(sf4e_recovery PUBLIC cxx_std_17)
# XInput is loaded at run time; DirectInput 8 is on every supported Windows.
target_link_libraries(sf4e_recovery PUBLIC sf4e_ui sf4e_app_services sf4e_localization PRIVATE d3d9 ole32 shell32 dinput8 dxguid)
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
add_library(sf4e_package_installer STATIC src/launcher/update/PackageInstaller.cxx src/launcher/update/ZipExtract.cxx)
target_compile_features(sf4e_package_installer PUBLIC cxx_std_17)
target_link_libraries(sf4e_package_installer PUBLIC sf4e_selection nlohmann_json::nlohmann_json bcrypt PRIVATE sf4e_durable_file ZLIB::ZLIB)
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
# write only the files whose content changed. The selection art is copied by the
# install rule's filter, which leaves out the photographs the runtime never reads.
add_custom_target(SelectionAssets
    COMMAND ${CMAKE_COMMAND} "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/assets/selection"
        "-DDEST=$<TARGET_FILE_DIR:Sidecar>/assets/selection" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/sf4e_copy_selection_art.cmake"
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
# The tests of the game libraries and the application, then of the helper and the room host,
# once every library and executable they link is defined.
include("${CMAKE_SOURCE_DIR}/cmake/sf4e_test_targets.cmake")
include("${CMAKE_SOURCE_DIR}/cmake/sf4e_network_tests.cmake")
# Registration is over: every test carries a label (cmake/sf4e_tests.cmake).
sf4e_check_test_labels()
