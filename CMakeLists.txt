cmake_minimum_required (VERSION 3.20)

include("${CMAKE_SOURCE_DIR}/cmake/sf4e_version.cmake")

project (
    sf4e
    VERSION 0.6.0
    DESCRIPTION "A process-inspection and modification tool for the Steam release of Ultra Street Fighter 4."
    HOMEPAGE_URL "https://github.com/Confetti3/SF4-Netplay-Launcher"
)

include(CTest)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG")
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)
add_compile_definitions(WIN32_LEAN_AND_MEAN)

find_library(LIB_DETOURS "detours")
find_package(CLI11 CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)
find_package(ggpo CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(wil CONFIG REQUIRED)

option(SF4E_LAUNCHER_QT_UI "Use Qt Widgets for the launcher UI" ON)

if(SF4E_LAUNCHER_QT_UI)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
    find_package(Qt6 CONFIG REQUIRED COMPONENTS Widgets)
endif()

find_path(VALVEFILEVDF_INCLUDE_DIRS "vdf_parser.hpp")

add_library(Dimps STATIC
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
    PUBLIC nlohmann_json::nlohmann_json
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
)
target_include_directories(sf4e PUBLIC "src/common")
target_link_libraries(
    sf4e
    PRIVATE ${LIB_DETOURS} Session sf4e_common "Kernel32" "ws2_32" "Mswsock" "AdvApi32" "Shlwapi" "pathcch" "shell32" "Bcrypt" "Xinput9_1_0" "iphlpapi"
    PUBLIC imgui::imgui nlohmann_json::nlohmann_json spdlog::spdlog GameNetworkingSockets::shared ggpo::GGPO
)

add_library(Session STATIC
    "src/session/sf4e__SessionClient.hxx"
    "src/session/sf4e__SessionClient.cxx"
    "src/session/sf4e__SessionProtocol.hxx"
    "src/session/sf4e__SessionProtocol.cxx"
    "src/session/sf4e__SessionServer.hxx"
    "src/session/sf4e__SessionServer.cxx"
    "src/session/sf4e__GgpoRelay.hxx"
    "src/session/sf4e__GgpoRelay.cxx"
    "src/session/sf4e__GgpoTransport.hxx"
    "src/session/sf4e__GgpoTransport.cxx"
)
target_link_libraries(
    Session
    PRIVATE Dimps sf4e_common
    PUBLIC nlohmann_json::nlohmann_json spdlog::spdlog GameNetworkingSockets::shared "ws2_32"
)

add_executable(SessionInteractiveTest "src/session/interactive_test.cxx")
target_link_libraries(SessionInteractiveTest PRIVATE nlohmann_json::nlohmann_json imgui::imgui spdlog::spdlog Dimps sf4e Session "d3d9")

if(BUILD_TESTING)
    add_executable(GgpoUdpValidationTest "src/tests/ggpo_udp_validation_test.cxx")
    target_link_libraries(GgpoUdpValidationTest PRIVATE ggpo::GGPO "ws2_32")
    add_test(NAME GgpoUdpValidation COMMAND GgpoUdpValidationTest)
    set_tests_properties(GgpoUdpValidation PROPERTIES TIMEOUT 15)

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
endif()

add_executable(RelayHost "src/relay/relay_host.cxx")
target_include_directories(RelayHost PRIVATE "src/common" "src/session")
target_link_libraries(RelayHost PRIVATE
    CLI11::CLI11
    Session
    spdlog::spdlog
    GameNetworkingSockets::shared
    "bcrypt"
    "pathcch"
    "ws2_32"
)
set_target_properties(RelayHost PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

set(SF4E_LAUNCHER_SOURCES
    src/launcher/launcher.cxx
    src/launcher/netplay/netplay_wizard.cxx
    src/launcher/netplay/netplay_persist.cxx
    src/launcher/netplay/netplay_room_code.cxx
    src/launcher/netplay/netplay_launch_controller.cxx
    src/launcher/connect_strategy.cxx
    src/launcher/netplay/room_broker_client.cxx
    src/launcher/netplay/upnp_portmap.cxx
    src/launcher/relay/relay_host_spawn.cxx
    src/launcher/update/github_release_client.cxx
    src/launcher/ipc/electron_ipc.cxx
)
if(SF4E_LAUNCHER_QT_UI)
    list(APPEND SF4E_LAUNCHER_SOURCES
        src/launcher/qt/controller_bridge.cxx
        src/launcher/qt/launcher_common.cxx
        src/launcher/qt/launcher_theme.cxx
        src/launcher/qt/relay_netplay_window.cxx
        src/launcher/qt/qt_launcher_app.cxx
    )
endif()
add_executable(Launcher WIN32 ${SF4E_LAUNCHER_SOURCES})
set(_SF4E_LAUNCHER_EXE_TARGET Launcher)
if(SF4E_LAUNCHER_QT_UI)
    target_compile_definitions(Launcher PRIVATE SF4E_LAUNCHER_QT_UI=1)
    target_link_libraries(Launcher PRIVATE Qt6::Widgets)
endif()
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
sf4e_configure_windows_version(Sidecar "SF4 Netplay Sidecar (USF4 netplay hooks)" "Sidecar.dll" "VFT_DLL")

target_compile_definitions(Launcher PRIVATE -D_UNICODE -DUNICODE "SF4E_APP_VERSION=\"${PROJECT_VERSION}\"")
set_target_properties(Launcher PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
sf4e_configure_windows_version(Launcher "SF4 Netplay Launcher" "Launcher.exe" "VFT_APP")
sf4e_configure_windows_version(RelayHost "SF4 Netplay Relay Host" "RelayHost.exe" "VFT_APP")

add_executable(Updater
    src/launcher/updater.cxx
)
target_link_libraries(Updater PRIVATE "pathcch" "shell32")
set_target_properties(Updater PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
sf4e_configure_windows_version(Updater "SF4 Netplay Updater" "Updater.exe" "VFT_APP")

install(
  TARGETS Launcher Sidecar RelayHost Updater
  RUNTIME DESTINATION "."
)

if(SF4E_LAUNCHER_QT_UI)
  find_program(SF4E_WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${Qt6_DIR}/../../../tools/Qt6/bin")
  if(SF4E_WINDEPLOYQT_EXECUTABLE)
    add_custom_command(
      TARGET Launcher POST_BUILD
      COMMAND "${SF4E_WINDEPLOYQT_EXECUTABLE}"
        --no-translations
        --no-compiler-runtime
        "$<TARGET_FILE:Launcher>"
      COMMENT "Deploy Qt DLLs next to Launcher.exe"
    )
  endif()
  add_custom_command(
    TARGET Launcher POST_BUILD
    COMMAND PowerShell -NoProfile -ExecutionPolicy Bypass -File
      "${CMAKE_SOURCE_DIR}/scripts/sync-x86-qt-plugins.ps1"
      -Dir "$<SHELL_PATH:$<TARGET_FILE_DIR:Launcher>>"
      -BuildDir "$<SHELL_PATH:${CMAKE_BINARY_DIR}>"
    COMMENT "Deploy x86 Qt plugins (windeployqt is x64 and breaks qwindows.dll)"
  )
  file(WRITE "${CMAKE_BINARY_DIR}/generated/qt.conf"
    "[Paths]\nPrefix=.\nPlugins=plugins\n")
  add_custom_command(
    TARGET Launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_BINARY_DIR}/generated/qt.conf"
      "$<TARGET_FILE_DIR:Launcher>/qt.conf"
    COMMENT "Copy qt.conf next to Launcher.exe"
  )
  install(FILES "${CMAKE_BINARY_DIR}/generated/qt.conf" DESTINATION ".")
endif()

# vcpkg copies these next to the build outputs; bundle them for team installs.
set(SF4E_RUNTIME_DLLS
  spdlog.dll
  fmt.dll
  GameNetworkingSockets.dll
  GGPO.dll
  libcrypto-3.dll
  libprotobuf.dll
  abseil_dll.dll
)
foreach(_dll IN LISTS SF4E_RUNTIME_DLLS)
  install(
    FILES "${CMAKE_BINARY_DIR}/${_dll}"
    DESTINATION "."
    OPTIONAL
  )
endforeach()

set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VENDOR "The ${PROJECT_NAME} Dev Team")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_NAME} ${PROJECT_DESCRIPTION}")
