# Ember's game-neutral code, compiled as SFV Ember compiles its own: x64,
# C++20, /W4 /permissive- /utf-8, the DLL CRT (SFV's CMakeLists.txt). It is the
# check before the library is shared (the library plan, sfv-ember's
# docs/design/EMBER_LIBRARY.md: section 6, "Library on V", and section 7).
# Nothing links it; scripts/probe-x64.ps1 configures and builds it. SF4
# Ember's build does not read this folder.
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
    # Selection: SF4's roster, stages and installed content.
    common/FighterCatalog.cxx common/FighterCatalog.hxx common/StageCatalog.cxx common/StageCatalog.hxx
    common/StageValue.hxx common/InstalledContent.cxx common/InstalledContent.hxx
    common/SelectionAssetPath.hxx common/RandomFighter.hxx common/LobbyMusic.hxx
    ui/FighterSelector.cxx ui/FighterSelector.hxx ui/SelectionArt.cxx
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
    # Platform: the game's picture, display settings, custom content and language.
    platform/FrameGrab.cxx platform/FrameGrab.hxx platform/GameDisplaySettings.cxx
    platform/GameDisplaySettings.hxx platform/CustomContentFiles.cxx platform/CustomContentFiles.hxx
    platform/LocaleWindows.cxx
    # Training's UI over SF4's Training mode.
    ui/TrainingPanel.cxx ui/TrainingPanel.hxx ui/TrainingMeter.cxx ui/TrainingData.cxx
    ui/TrainingShortcuts.cxx
    # The UI lab runs the shell on Direct3D 9 with SF4's selector and Training
    # panel (section 7); SFV gets a Direct3D 11 lab of its own. The launcher's
    # recovery window draws on Direct3D 9 as well.
    ui/lab/UiLab.cxx ui/RecoverySurface.cxx ui/RecoverySurface.hxx
    # Session wire: SF4's plain types become the SF4 match codec, and the
    # client's state verification reads SF4's battle (section 1.3).
    session/SessionPlainTypes.hxx session/sf4e__SessionClient__Verification.cxx)

set(EMBER_NOT_FOR_WINDOWS
    # The Linux room host's helper client; Windows builds HelperClient.cxx.
    platform/HelperClientPosix.cxx)

# Neutral, but they reach SF4's battle types until the session wire is the
# library's (step 6): SessionPlainTypes.hxx includes Dimps' headers on
# Windows, and the room client also reaches SF4's save states. The
# ember_waiting_x64 target builds them (scripts/probe-x64.ps1 -Waiting) to
# show what is left; it is not part of the probe's default build.
set(EMBER_WAITING_STEP_6
    session/IrohMatchSession.cxx session/IrohMatchSession.hxx session/IrohRoom.cxx
    session/IrohRoomAdapters.cxx session/IrohRoomCheckpoint.cxx session/IrohRoomPeers.cxx
    session/MatchAuthority.cxx session/MatchAuthority.hxx session/RoomMessageTag.hxx
    session/RoomRecoveryRuntime.hxx session/sf4e__SessionClient.cxx session/sf4e__SessionClient.hxx
    session/sf4e__SessionClient__Room.cxx session/sf4e__SessionProtocol.cxx session/sf4e__SessionProtocol.hxx
    session/sf4e__SessionServer.cxx session/sf4e__SessionServer.hxx session/sf4e__SessionServer__Handlers.cxx
    session/sf4e__SessionServer__Recovery.cxx session/sf4e__SessionServer__RecoveryCheckpoint.cxx
    session/sf4e__SessionServer__Rooms.cxx session/sf4e__SessionServer__Tournament.cxx)

# A file named in a list but gone from the tree is a stale entry: stop.
foreach(_file IN LISTS EMBER_SF4_SIDE EMBER_NOT_FOR_WINDOWS EMBER_WAITING_STEP_6)
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
list(REMOVE_ITEM _ember_neutral ${EMBER_SF4_SIDE} ${EMBER_NOT_FOR_WINDOWS} ${EMBER_WAITING_STEP_6})
ember_probe_units("${_ember_neutral}" _ember_neutral_units)
ember_probe_units("${EMBER_WAITING_STEP_6}" _ember_waiting_units)
list(LENGTH _ember_neutral_units _neutral_count)
list(LENGTH _ember_waiting_units _waiting_count)
message(STATUS "ember-x64-probe: ${_neutral_count} neutral units, ${_waiting_count} waiting on step 6")

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
add_library(ember_waiting_x64 STATIC EXCLUDE_FROM_ALL ${_ember_waiting_units})
foreach(_target ember_neutral_x64 ember_waiting_x64)
    target_include_directories(${_target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/public" "${CMAKE_CURRENT_BINARY_DIR}/generated/identity")
    # Each product's build names its own version; the probe ships nothing.
    target_compile_definitions(${_target} PRIVATE SF4E_APP_VERSION="0.0.0-x64-probe")
    target_link_libraries(${_target} PRIVATE
        fmt::fmt ggpo::GGPO imgui::imgui nlohmann_json::nlohmann_json spdlog::spdlog ZLIB::ZLIB)
endforeach()
