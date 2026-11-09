# Ember's game-neutral code, compiled as SFV Ember compiles its own: x64,
# C++20, /W4 /permissive- /utf-8, the DLL CRT (SFV's CMakeLists.txt). It is the
# check before the library is shared (the library plan, sfv-ember's
# docs/design/EMBER_LIBRARY.md: section 6, "Library on V", and section 7).
# Only the runtime's test below links it; scripts/probe-x64.ps1 configures,
# builds and runs it. SF4 Ember's build does not read this folder.
#
# The neutral set is every source in the library's folders except SF4's side
# (the plan's section 1.2), listed below with the reason for each group of
# files. A new file is therefore neutral until it is listed here, so it has to
# build for SFV as well. Every neutral header is also compiled on its own, so
# a header that needs an include it does not make, or that holds only on the
# 32-bit game, fails here.
cmake_minimum_required(VERSION 3.26)
project(ember_x64_probe LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The probe builds for x64 only (scripts/probe-x64.ps1)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
# SFV's flags, plus /WX so that a warning fails the probe instead of passing
# unread. The packages' own headers are not Ember's to fix.
add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /WX /external:anglebrackets /external:W0)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)

find_package(fmt CONFIG REQUIRED)
find_package(ggpo CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

get_filename_component(EMBER_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../src" ABSOLUTE)

# The library's folders. launcher/ and training/ hold more of SF4 than of the
# library, so only their neutral parts are named. The launcher program itself
# (launcher.cxx, the recovery window and updater.cxx) is built further down.
file(GLOB_RECURSE _ember_files CONFIGURE_DEPENDS RELATIVE "${EMBER_SOURCE_ROOT}"
    "${EMBER_SOURCE_ROOT}/common/*" "${EMBER_SOURCE_ROOT}/netplay/*" "${EMBER_SOURCE_ROOT}/platform/*"
    "${EMBER_SOURCE_ROOT}/session/*" "${EMBER_SOURCE_ROOT}/ui/*" "${EMBER_SOURCE_ROOT}/launcher/update/*")
list(APPEND _ember_files
    launcher/RunningGameFocus.hxx launcher/GameLocator.cxx launcher/GameLocator.hxx launcher/LaunchSteps.hxx
    launcher/netplay/netplay_persist.cxx launcher/netplay/netplay_persist.hxx
    discord/BridgeServer.hxx discord/Presence.hxx discord/Ticket.hxx
    training/FrameMeter.hxx training/MoveInputs.hxx training/PracticeSettings.hxx
    training/RecordingBank.hxx training/RecordingFile.hxx training/RecordingTypes.hxx
    training/TrainingSession.hxx)

set(EMBER_SF4_SIDE
    # Selection: SF4's roster, stages, installed content and packaged art.
    common/FighterCatalog.cxx common/FighterCatalog.hxx common/StageCatalog.cxx common/StageCatalog.hxx
    common/StageValue.hxx common/InstalledContent.cxx common/InstalledContent.hxx
    common/Sf4SelectionAssetPath.cxx common/LobbyMusic.hxx
    ui/FighterSelector.cxx ui/FighterSelector.hxx ui/AppearanceGallery.cxx ui/AppearanceGallery.hxx
    ui/Sf4SelectionLabels.cxx ui/Sf4SelectionArt.cxx
    ui/SelectionArtD3D9.cxx ui/SelectionArtD3D9.hxx
    # Language, display and pads: the game's files and its pad layer.
    common/GameLanguage.cxx common/GameLanguage.hxx
    common/GameDisplayConfig.hxx common/DirectInputState.hxx
    common/GamepadBrand.hxx common/ControllerView.hxx common/TrainingPad.hxx common/TrainingStage.hxx
    # Layouts of the 32-bit engine's keyboard list and replay recorder, held
    # by static_asserts that only the 32-bit game meets (section 7).
    common/KeyboardSlots.hxx common/ReplayRecorder.hxx
    # Replays and rollback state: the Battle Log format, its files and the
    # engine's mementos.
    common/ReplayExport.hxx common/InPlaceReplays.hxx common/NativeMatchResult.hxx
    common/NativeMenuLock.hxx common/RollbackAudit.hxx common/AfterimageMemento.hxx
    common/VfxObjectMemento.hxx common/HudMementoQueue.hxx common/MementoGuards.hxx
    platform/ReplayFiles.cxx platform/ReplayFiles.hxx platform/ReplayArchiveIndex.hxx
    platform/ReplayArchivePass.hxx platform/ReplayArchiveWorker.hxx platform/ReplayImportWorker.hxx
    platform/ReplayNotes.hxx platform/ReplayPath.hxx platform/ReplayRecordingWorker.hxx
    platform/ReplaySender.hxx platform/ReplaySetPack.hxx platform/ReplaySets.hxx
    # Platform: the game's picture, display settings and custom content.
    platform/FrameGrab.cxx platform/FrameGrab.hxx platform/GameDisplaySettings.cxx
    platform/GameDisplaySettings.hxx platform/CustomContentFiles.cxx platform/CustomContentFiles.hxx
    # Training's UI over SF4's Training mode.
    ui/TrainingPanel.cxx ui/TrainingPanel.hxx ui/TrainingMeter.cxx ui/TrainingData.cxx
    ui/TrainingShortcuts.cxx
    # The UI lab runs the shell on Direct3D 9 with SF4's selector and Training
    # panel (section 7); SFV gets a Direct3D 11 lab of its own.
    ui/lab/UiLab.cxx)

# The launcher's recovery window, on Direct3D 9: built into the launcher's
# library below, not the neutral one, so the game's own code never links
# Direct3D 9.
set(EMBER_LAUNCHER_UI
    ui/RecoverySurface.cxx ui/RecoverySurface.hxx ui/RecoveryController.cxx
    ui/SelectionArtD3D9.cxx ui/SelectionArtD3D9.hxx)

set(EMBER_NOT_FOR_WINDOWS
    # The Linux room host's helper client; Windows builds HelperClient.cxx.
    platform/HelperClientPosix.cxx)

# A file named in a list but gone from the tree is a stale entry: stop.
foreach(_file IN LISTS EMBER_SF4_SIDE EMBER_NOT_FOR_WINDOWS EMBER_LAUNCHER_UI)
    if(NOT EXISTS "${EMBER_SOURCE_ROOT}/${_file}")
        message(FATAL_ERROR "ember-x64-probe: '${_file}' is listed but is not in src/")
    endif()
endforeach()

# Sources as they are; each header as a translation unit of its own.
function(ember_probe_units files out_var)
    set(units "")
    foreach(file IN LISTS files)
        if(file MATCHES "\\.(cxx|cpp)$")
            list(APPEND units "${EMBER_SOURCE_ROOT}/${file}")
        elseif(file MATCHES "\\.(hxx|h)$")
            string(REPLACE "/" "__" unit "${file}")
            set(unit "${CMAKE_CURRENT_BINARY_DIR}/header-units/${unit}.cxx")
            file(CONFIGURE OUTPUT "${unit}" CONTENT "#include \"${EMBER_SOURCE_ROOT}/${file}\"\n")
            list(APPEND units "${unit}")
        endif()
    endforeach()
    set(${out_var} "${units}" PARENT_SCOPE)
endfunction()

set(_ember_neutral "${_ember_files}")
list(REMOVE_ITEM _ember_neutral ${EMBER_SF4_SIDE} ${EMBER_NOT_FOR_WINDOWS} ${EMBER_LAUNCHER_UI})
ember_probe_units("${_ember_neutral}" _ember_neutral_units)
list(LENGTH _ember_neutral_units _neutral_count)
message(STATUS "ember-x64-probe: ${_neutral_count} neutral units")

# What the library's build generates, as SF4 Ember's does: the locale
# catalogs, fonts, chat filter lists and speech relay script, and the
# source revision the logs and problem reports name.
include("${CMAKE_CURRENT_SOURCE_DIR}/../sf4e_locales.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../sf4e_fonts.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../sf4e_chat_filter.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../sf4e_speech_relay.cmake")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${EMBER_SOURCE_ROOT}"
    "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/identity/BuildIdentity.hxx"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/../sf4e_build_identity.cmake" COMMAND_ERROR_IS_FATAL ANY)

add_library(ember_neutral_x64 STATIC ${_ember_neutral_units})
# Ember's Win32 backend (ui/backends), which a game's overlay uses in place
# of the package's: the pads reach the menu through the runtime's controller
# sample, never through ImGui's own XInput reads, as in SF4 Ember's build.
set_source_files_properties("${EMBER_SOURCE_ROOT}/ui/backends/imgui_impl_win32.cpp" PROPERTIES
    COMPILE_DEFINITIONS IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
# The locale ids (generated/public) are part of the library's headers, as
# sf4e_localization gives them in SF4 Ember's build.
target_include_directories(ember_neutral_x64 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/identity" PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated/public")
# Each product's build names its own version: a game that builds the library
# from here sets EMBER_APP_VERSION before it adds this folder. The probe on
# its own ships nothing.
if(NOT DEFINED EMBER_APP_VERSION)
    set(EMBER_APP_VERSION "0.0.0-x64-probe")
endif()
target_compile_definitions(ember_neutral_x64 PRIVATE SF4E_APP_VERSION="${EMBER_APP_VERSION}")
target_link_libraries(ember_neutral_x64 PRIVATE
    fmt::fmt ggpo::GGPO imgui::imgui nlohmann_json::nlohmann_json spdlog::spdlog ZLIB::ZLIB)
# The product's package inventory (common/PackageInventory.hxx): a game that
# ships its own package names its file before it adds this folder; the probe
# on its own compiles SF4 Ember's.
set(EMBER_PACKAGE_INVENTORY "" CACHE FILEPATH "The product's package inventory, in PackageInventory.inc's form (empty: SF4 Ember's)")
if(EMBER_PACKAGE_INVENTORY)
    if(NOT EXISTS "${EMBER_PACKAGE_INVENTORY}")
        message(FATAL_ERROR "EMBER_PACKAGE_INVENTORY names '${EMBER_PACKAGE_INVENTORY}', which does not exist")
    endif()
    target_compile_definitions(ember_neutral_x64 PUBLIC "EMBER_PACKAGE_INVENTORY=\"${EMBER_PACKAGE_INVENTORY}\"")
endif()

# The runtime (sf4e_runtime), compiled as SFV Ember would build it, and the
# runtime's test under the fake game (RuntimeCapabilitiesTest) linked against
# it; scripts/probe-x64.ps1 runs the test. It links with nothing of SF4's
# side: the game's language, pick labels, pictures and replay archive are the
# fake game's (section 9 of sfv-ember's EMBER_LIBRARY.md), and so are its catalog
# and its packaged selection files (fake_catalog.cxx, fake_selection.cxx: step 5).
# Only the product identity is still SF4's.
include("${CMAKE_CURRENT_SOURCE_DIR}/../sf4e_runtime_sources.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../sf4e_product.cmake")
list(FILTER SF4E_RUNTIME_SOURCES INCLUDE REGEX "\\.cxx$")
list(TRANSFORM SF4E_RUNTIME_SOURCES PREPEND "${EMBER_SOURCE_ROOT}/" OUTPUT_VARIABLE _ember_runtime_units)
add_library(ember_runtime_x64 STATIC ${_ember_runtime_units})
target_include_directories(ember_runtime_x64 PRIVATE "${EMBER_SOURCE_ROOT}/common"
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${CMAKE_CURRENT_BINARY_DIR}/generated/identity")
target_compile_definitions(ember_runtime_x64 PRIVATE SF4E_APP_VERSION="${EMBER_APP_VERSION}")
target_link_libraries(ember_runtime_x64 PUBLIC ember_neutral_x64
    fmt::fmt ggpo::GGPO imgui::imgui nlohmann_json::nlohmann_json spdlog::spdlog ZLIB::ZLIB
    Kernel32 ws2_32 Shlwapi pathcch shell32 Bcrypt iphlpapi winmm ole32 windowscodecs)

# The test's game (fake_*.cxx: a game adapter, catalog, desync check, pick labels and
# pictures with nothing of SF4's) is compiled into the test alone, so a game
# that links the library defines its own Game() and Catalog() with no second
# one beside them.
add_executable(ember_runtime_x64_test
    "${EMBER_SOURCE_ROOT}/tests/runtime_capabilities_test.cxx"
    "${EMBER_SOURCE_ROOT}/tests/fake_game.cxx"
    "${EMBER_SOURCE_ROOT}/tests/fake_session_verification.cxx"
    "${EMBER_SOURCE_ROOT}/tests/fake_selection.cxx"
    "${EMBER_SOURCE_ROOT}/tests/fake_catalog.cxx")
sf4e_add_product_identity(ember_runtime_x64_test)
target_include_directories(ember_runtime_x64_test PRIVATE "${EMBER_SOURCE_ROOT}/common")
target_link_libraries(ember_runtime_x64_test PRIVATE ember_runtime_x64)

# The launcher and updater programs (launcher.cxx, updater.cxx) and the
# launcher's recovery window, as libraries: a game builds Launcher.exe and
# Updater.exe from them with its product identity, its launch steps
# (launcher/LaunchSteps.hxx), its catalog and its pick labels and pictures,
# as SF4 Ember's build does with its own. They hold each program's entry
# point, so each goes into one executable only.
find_package(CLI11 CONFIG REQUIRED)
set(_ember_launcher_ui ${EMBER_LAUNCHER_UI})
list(FILTER _ember_launcher_ui INCLUDE REGEX "\\.cxx$")
list(TRANSFORM _ember_launcher_ui PREPEND "${EMBER_SOURCE_ROOT}/")
add_library(ember_launcher_x64 STATIC "${EMBER_SOURCE_ROOT}/launcher/launcher.cxx" ${_ember_launcher_ui})
target_compile_definitions(ember_launcher_x64 PRIVATE UNICODE _UNICODE SF4E_APP_VERSION="${EMBER_APP_VERSION}")
target_include_directories(ember_launcher_x64 PRIVATE "${EMBER_SOURCE_ROOT}/common" "${EMBER_SOURCE_ROOT}/launcher"
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${CMAKE_CURRENT_BINARY_DIR}/generated/identity")
target_link_libraries(ember_launcher_x64 PUBLIC ember_neutral_x64
    CLI11::CLI11 fmt::fmt imgui::imgui nlohmann_json::nlohmann_json spdlog::spdlog ZLIB::ZLIB
    d3d9 dinput8 dxguid shlwapi pathcch version dbghelp bcrypt shell32 ole32 oleaut32 user32 gdi32 ws2_32
    winhttp iphlpapi winmm windowscodecs comctl32
    # The video export's encoder (platform/VideoLinkServe.cxx), which runs in
    # the launcher's own process, loaded only when it is used, as in SF4
    # Ember's build (cmake/sf4e_video.cmake).
    mfplat mfreadwrite mfuuid mmdevapi d3d11 dxgi delayimp)
target_link_options(ember_launcher_x64 INTERFACE "/DELAYLOAD:mfplat.dll" "/DELAYLOAD:mfreadwrite.dll"
    "/DELAYLOAD:MMDevAPI.dll" "/DELAYLOAD:d3d11.dll" "/DELAYLOAD:dxgi.dll")
add_library(ember_updater_x64 STATIC "${EMBER_SOURCE_ROOT}/launcher/updater.cxx")
target_link_libraries(ember_updater_x64 PUBLIC ember_neutral_x64 nlohmann_json::nlohmann_json ZLIB::ZLIB
    bcrypt pathcch shell32 comctl32 user32)
