cmake_minimum_required (VERSION 3.20)

include("${CMAKE_SOURCE_DIR}/cmake/sf4e_version.cmake")

project (
    sf4e
    VERSION 0.3.6
    DESCRIPTION "A process-inspection and modification tool for the Steam release of Ultra Street Fighter 4."
    HOMEPAGE_URL "https://github.com/Confetti3/SF4-Netplay-Launcher"
)

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

option(SF4E_LAUNCHER_QT_UI "Use Qt Widgets for the launcher UI (Steam experiment)" OFF)
if(SF4E_ENABLE_STEAMWORKS_EXPERIMENT)
    set(SF4E_LAUNCHER_QT_UI ON CACHE BOOL "Use Qt Widgets for the launcher UI (Steam experiment)" FORCE)
endif()

if(SF4E_LAUNCHER_QT_UI)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
    find_package(Qt6 CONFIG REQUIRED COMPONENTS Widgets)
else()
    find_package(unofficial-webview2 CONFIG REQUIRED)
endif()

find_path(VALVEFILEVDF_INCLUDE_DIRS "vdf_parser.hpp")

option(SF4E_ENABLE_STEAMWORKS_EXPERIMENT "Build experimental Steamworks P2P probe (off by default)" OFF)
set(SF4E_STEAMWORKS_SDK_DIR "" CACHE PATH "Path to an unpacked Steamworks SDK, e.g. C:/.../sdk")

if(SF4E_ENABLE_STEAMWORKS_EXPERIMENT)
    if(NOT SF4E_STEAMWORKS_SDK_DIR)
        message(FATAL_ERROR "SF4E_ENABLE_STEAMWORKS_EXPERIMENT requires SF4E_STEAMWORKS_SDK_DIR")
    endif()
    set(SF4E_STEAMWORKS_INCLUDE_DIR "${SF4E_STEAMWORKS_SDK_DIR}/public")
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(SF4E_STEAMWORKS_LIB "${SF4E_STEAMWORKS_SDK_DIR}/redistributable_bin/win64/steam_api64.lib")
        set(SF4E_STEAMWORKS_DLL "${SF4E_STEAMWORKS_SDK_DIR}/redistributable_bin/win64/steam_api64.dll")
    else()
        set(SF4E_STEAMWORKS_LIB "${SF4E_STEAMWORKS_SDK_DIR}/redistributable_bin/steam_api.lib")
        set(SF4E_STEAMWORKS_DLL "${SF4E_STEAMWORKS_SDK_DIR}/redistributable_bin/steam_api.dll")
    endif()
    if(NOT EXISTS "${SF4E_STEAMWORKS_INCLUDE_DIR}/steam/steam_api.h")
        message(FATAL_ERROR "Steamworks headers not found under ${SF4E_STEAMWORKS_INCLUDE_DIR}")
    endif()
    if(NOT EXISTS "${SF4E_STEAMWORKS_LIB}")
        message(FATAL_ERROR "Steamworks import library not found: ${SF4E_STEAMWORKS_LIB}")
    endif()
    if(NOT EXISTS "${SF4E_STEAMWORKS_DLL}")
        message(FATAL_ERROR "Steamworks runtime DLL not found: ${SF4E_STEAMWORKS_DLL}")
    endif()
    add_library(SteamworksSDK UNKNOWN IMPORTED)
    set_target_properties(SteamworksSDK PROPERTIES
        IMPORTED_LOCATION "${SF4E_STEAMWORKS_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${SF4E_STEAMWORKS_INCLUDE_DIR}"
    )
endif()

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
if(SF4E_ENABLE_STEAMWORKS_EXPERIMENT)
    target_compile_definitions(sf4e PRIVATE SF4E_STEAMWORKS_EXPERIMENT=1)
endif()
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
if(SF4E_ENABLE_STEAMWORKS_EXPERIMENT)
    target_sources(Session PRIVATE
        src/session/sf4e__SteamP2pSession.hxx
        src/session/sf4e__SteamP2pSession.cxx
    )
    target_compile_definitions(Session PRIVATE SF4E_STEAMWORKS_EXPERIMENT=1)
    target_include_directories(Session PRIVATE "${SF4E_STEAMWORKS_INCLUDE_DIR}")
    target_link_libraries(Session PRIVATE SteamworksSDK)
endif()
target_link_libraries(
    Session
    PRIVATE Dimps sf4e_common
    PUBLIC nlohmann_json::nlohmann_json spdlog::spdlog GameNetworkingSockets::shared "ws2_32"
)

add_executable(SessionInteractiveTest "src/session/interactive_test.cxx")
target_link_libraries(SessionInteractiveTest PRIVATE nlohmann_json::nlohmann_json imgui::imgui spdlog::spdlog Dimps sf4e Session "d3d9")

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
        src/launcher/qt/steam_p2p_window.cxx
        src/launcher/qt/qt_launcher_app.cxx
    )
else()
    list(APPEND SF4E_LAUNCHER_SOURCES src/launcher/webview/webview_host.cxx)
endif()
if(SF4E_LAUNCHER_QT_UI)
    add_executable(LauncherApp WIN32 ${SF4E_LAUNCHER_SOURCES})
    set(_SF4E_LAUNCHER_EXE_TARGET LauncherApp)
else()
    add_executable(Launcher WIN32 ${SF4E_LAUNCHER_SOURCES})
    set(_SF4E_LAUNCHER_EXE_TARGET Launcher)
endif()
if(SF4E_ENABLE_STEAMWORKS_EXPERIMENT)
    target_sources(${_SF4E_LAUNCHER_EXE_TARGET} PRIVATE
        src/launcher/steam/steam_p2p_launcher.hxx
        src/launcher/steam/steam_p2p_launcher.cxx
        src/steam_experiment/steam_p2p_payload.hxx
        src/steam_experiment/steam_p2p_payload.cxx
    )
    target_compile_definitions(${_SF4E_LAUNCHER_EXE_TARGET} PRIVATE SF4E_STEAMWORKS_EXPERIMENT=1)
endif()
if(SF4E_LAUNCHER_QT_UI)
    target_compile_definitions(LauncherApp PRIVATE SF4E_LAUNCHER_QT_UI=1)
    target_link_libraries(LauncherApp PRIVATE Qt6::Widgets)
    add_executable(Launcher WIN32 src/launcher/launcher_stub.cxx)
    target_link_libraries(Launcher PRIVATE sf4e_common shlwapi pathcch shell32)
    target_compile_definitions(Launcher PRIVATE -D_UNICODE -DUNICODE)
    sf4e_configure_windows_version(Launcher "SF4 Netplay Launcher" "Launcher.exe" "VFT_APP")
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
if(NOT SF4E_LAUNCHER_QT_UI)
    target_link_libraries(${_SF4E_LAUNCHER_EXE_TARGET} PRIVATE unofficial::webview2::webview2)
endif()
if(SF4E_ENABLE_STEAMWORKS_EXPERIMENT)
    target_link_libraries(${_SF4E_LAUNCHER_EXE_TARGET} PRIVATE SteamworksSDK GameNetworkingSockets::shared)
    add_custom_command(
        TARGET ${_SF4E_LAUNCHER_EXE_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SF4E_STEAMWORKS_DLL}"
            "$<TARGET_FILE_DIR:${_SF4E_LAUNCHER_EXE_TARGET}>"
        COMMENT "Copy Steamworks runtime next to launcher app"
    )
endif()

add_library(Sidecar SHARED src/sidecar/sidecar.cxx)
target_include_directories(Sidecar PRIVATE ${VALVEFILEVDF_INCLUDE_DIRS})
target_link_libraries(Sidecar PRIVATE ${LIB_DETOURS} Dimps sf4e)
if(SF4E_ENABLE_STEAMWORKS_EXPERIMENT)
    target_compile_definitions(Sidecar PRIVATE SF4E_STEAMWORKS_EXPERIMENT=1)
    if(MSVC)
        target_link_libraries(Sidecar PRIVATE delayimp)
        target_link_options(Sidecar PRIVATE "/DELAYLOAD:steam_api.dll")
    endif()
    add_custom_command(
        TARGET Sidecar POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SF4E_STEAMWORKS_DLL}"
            "$<TARGET_FILE_DIR:Sidecar>"
        COMMENT "Copy Steamworks runtime next to Sidecar.dll"
    )
endif()
sf4e_configure_windows_version(Sidecar "SF4 Netplay Sidecar (USF4 netplay hooks)" "Sidecar.dll" "VFT_DLL")

if(NOT SF4E_LAUNCHER_QT_UI)
    target_compile_definitions(Launcher PRIVATE -D_UNICODE -DUNICODE)
endif()
set_target_properties(${_SF4E_LAUNCHER_EXE_TARGET} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
if(NOT SF4E_LAUNCHER_QT_UI)
    sf4e_configure_windows_version(Launcher "SF4 Netplay Launcher" "Launcher.exe" "VFT_APP")
else()
    sf4e_configure_windows_version(LauncherApp "SF4 Netplay Launcher (Qt)" "LauncherApp.exe" "VFT_APP")
endif()
sf4e_configure_windows_version(RelayHost "SF4 Netplay Relay Host" "RelayHost.exe" "VFT_APP")

add_executable(Updater
    src/launcher/updater.cxx
)
target_link_libraries(Updater PRIVATE "pathcch" "shell32")
set_target_properties(Updater PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
sf4e_configure_windows_version(Updater "SF4 Netplay Updater" "Updater.exe" "VFT_APP")

add_executable(SteamP2PPayloadTest
    src/steam_experiment/steam_p2p_payload.hxx
    src/steam_experiment/steam_p2p_payload.cxx
    src/steam_experiment/steam_p2p_payload_test.cxx
)
target_link_libraries(SteamP2PPayloadTest PRIVATE nlohmann_json::nlohmann_json)
set_target_properties(SteamP2PPayloadTest PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
enable_testing()
add_test(NAME SteamP2PPayloadTest COMMAND SteamP2PPayloadTest)

if(SF4E_ENABLE_STEAMWORKS_EXPERIMENT)
    add_executable(SteamP2PProbe
        src/steam_experiment/steam_p2p_payload.hxx
        src/steam_experiment/steam_p2p_payload.cxx
        src/steam_experiment/steam_p2p_probe.cxx
    )
    target_link_libraries(SteamP2PProbe PRIVATE
        CLI11::CLI11
        nlohmann_json::nlohmann_json
        SteamworksSDK
        GameNetworkingSockets::shared
        "ws2_32"
    )
    set_target_properties(SteamP2PProbe PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    add_custom_command(
        TARGET SteamP2PProbe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SF4E_STEAMWORKS_DLL}"
            "$<TARGET_FILE_DIR:SteamP2PProbe>"
        COMMENT "Copy Steamworks runtime next to SteamP2PProbe"
    )
    install(TARGETS SteamP2PProbe RUNTIME DESTINATION "steam-experiment")
    install(FILES "${SF4E_STEAMWORKS_DLL}" DESTINATION "steam-experiment")
endif()

install(
  TARGETS Launcher Sidecar RelayHost Updater
  RUNTIME DESTINATION "."
)
if(SF4E_ENABLE_STEAMWORKS_EXPERIMENT)
  install(FILES "${SF4E_STEAMWORKS_DLL}" DESTINATION ".")
endif()

if(NOT SF4E_LAUNCHER_QT_UI)
  install(DIRECTORY "${CMAKE_SOURCE_DIR}/launcher-ui" DESTINATION ".")
  get_target_property(WEBVIEW2_LOADER_DLL unofficial::webview2::webview2 IMPORTED_LOCATION)
  if(WEBVIEW2_LOADER_DLL)
    install(FILES "${WEBVIEW2_LOADER_DLL}" DESTINATION ".")
    add_custom_command(
      TARGET Launcher POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WEBVIEW2_LOADER_DLL}"
        "$<TARGET_FILE_DIR:Launcher>"
      COMMENT "Copy WebView2Loader.dll next to Launcher.exe"
    )
    add_custom_command(
      TARGET Launcher POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/launcher-ui"
        "$<TARGET_FILE_DIR:Launcher>/launcher-ui"
      COMMENT "Copy launcher-ui next to Launcher.exe"
    )
  endif()
endif()
if(SF4E_LAUNCHER_QT_UI)
  find_program(SF4E_WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${Qt6_DIR}/../../../tools/Qt6/bin")
  if(SF4E_WINDEPLOYQT_EXECUTABLE)
    add_custom_command(
      TARGET LauncherApp POST_BUILD
      COMMAND "${SF4E_WINDEPLOYQT_EXECUTABLE}"
        --no-translations
        --no-compiler-runtime
        "$<TARGET_FILE:LauncherApp>"
      COMMENT "Deploy Qt DLLs next to LauncherApp.exe"
    )
  endif()
  add_custom_command(
    TARGET LauncherApp POST_BUILD
    COMMAND PowerShell -NoProfile -ExecutionPolicy Bypass -File
      "${CMAKE_SOURCE_DIR}/scripts/sync-x86-qt-plugins.ps1"
      -Dir "$<SHELL_PATH:$<TARGET_FILE_DIR:LauncherApp>>"
      -BuildDir "$<SHELL_PATH:${CMAKE_BINARY_DIR}>"
    COMMENT "Deploy x86 Qt plugins (windeployqt is x64 and breaks qwindows.dll)"
  )
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
