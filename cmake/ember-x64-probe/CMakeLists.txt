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
# library, so only their neutral parts are named.
file(GLOB_RECURSE _ember_files CONFIGURE_DEPENDS RELATIVE "${EMBER_SOURCE_ROOT}"
    "${EMBER_SOURCE_ROOT}/common/*" "${EMBER_SOURCE_ROOT}/netplay/*" "${EMBER_SOURCE_ROOT}/platform/*"
    "${EMBER_SOURCE_ROOT}/session/*" "${EMBER_SOURCE_ROOT}/ui/*" "${EMBER_SOURCE_ROOT}/launcher/update/*")
list(APPEND _ember_files
    launcher/RunningGameFocus.hxx
    discord/BridgeServer.hxx discord/Presence.hxx discord/Ticket.hxx
    training/FrameMeter.hxx training/MoveInputs.hxx training/PracticeSettings.hxx
    training/RecordingBank.hxx training/RecordingFile.hxx training/RecordingTypes.hxx
    training/TrainingSession.hxx)

set(EMBER_SF4_SIDE
    # Selection: SF4's roster, stages, installed content and packaged art.
    common/FighterCatalog.cxx common/FighterCatalog.hxx common/StageCatalog.cxx common/StageCatalog.hxx
    common/StageValue.hxx common/InstalledContent.cxx common/InstalledContent.hxx
    common/Sf4SelectionAssetPath.cxx common/LobbyMusic.hxx
    ui/FighterSelector.cxx ui/FighterSelector.hxx ui/Sf4SelectionLabels.cxx ui/Sf4SelectionArt.cxx
    ui/SelectionArtD3D9.cxx ui/SelectionArtD3D9.hxx
    # Language, display and pads: the game's files and its pad layer.
    common/GameLanguage.cxx common/GameLanguage.hxx common/SteamKeyValues.cxx common/SteamKeyValues.hxx
    common/GameDisplayConfig.hxx common/GamepadSlots.hxx common/DirectInputState.hxx
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
    # panel (section 7); SFV gets a Direct3D 11 lab of its own. The launcher's
    # recovery window draws on Direct3D 9 as well.
    ui/lab/UiLab.cxx ui/RecoverySurface.cxx ui/RecoverySurface.hxx)

set(EMBER_NOT_FOR_WINDOWS
    # The Linux room host's helper client; Windows builds HelperClient.cxx.
    platform/HelperClientPosix.cxx)

# A file named in a list but gone from the tree is a stale entry: stop.
foreach(_file IN LISTS EMBER_SF4_SIDE EMBER_NOT_FOR_WINDOWS)
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
list(REMOVE_ITEM _ember_neutral ${EMBER_SF4_SIDE} ${EMBER_NOT_FOR_WINDOWS})
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
# The locale ids (generated/public) are part of the library's headers, as
# sf4e_localization gives them in SF4 Ember's build.
target_include_directories(ember_neutral_x64 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/identity" PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated/public")
# Each product's build names its own version; the probe ships nothing.
target_compile_definitions(ember_neutral_x64 PRIVATE SF4E_APP_VERSION="0.0.0-x64-probe")
target_link_libraries(ember_neutral_x64 PRIVATE
    fmt::fmt ggpo::GGPO imgui::imgui nlohmann_json::nlohmann_json spdlog::spdlog ZLIB::ZLIB)

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
target_compile_definitions(ember_runtime_x64 PRIVATE SF4E_APP_VERSION="0.0.0-x64-probe")
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
