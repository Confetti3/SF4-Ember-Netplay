cmake_minimum_required(VERSION 3.20)
project(sf4e_session_relay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CLI11 CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

set(SF4E_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/src/session/sf4e__SessionServer.cxx")
  set(SF4E_ROOT "${CMAKE_CURRENT_LIST_DIR}")
endif()

set(LINUX_SHIM "${CMAKE_CURRENT_LIST_DIR}/linux-shim")

add_library(SessionRelay STATIC
  "${SF4E_ROOT}/src/session/sf4e__SessionProtocol.cxx"
  "${SF4E_ROOT}/src/session/sf4e__SessionServer.cxx"
)
target_include_directories(SessionRelay PUBLIC
  "${SF4E_ROOT}/src/session"
  "${SF4E_ROOT}/src/common"
)
target_link_libraries(SessionRelay PUBLIC
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  GameNetworkingSockets::shared
)

if(UNIX AND NOT APPLE)
  target_include_directories(SessionRelay SYSTEM BEFORE PUBLIC "${LINUX_SHIM}")
endif()

add_executable(sf4e-session-relay "${SF4E_ROOT}/src/relay/session_relay.cxx")
target_include_directories(sf4e-session-relay PRIVATE
  "${SF4E_ROOT}/src/session"
)
if(UNIX AND NOT APPLE)
  target_include_directories(sf4e-session-relay SYSTEM BEFORE PUBLIC "${LINUX_SHIM}")
endif()
target_link_libraries(sf4e-session-relay PRIVATE
  CLI11::CLI11
  SessionRelay
  spdlog::spdlog
  GameNetworkingSockets::shared
)

install(TARGETS sf4e-session-relay RUNTIME DESTINATION bin)

add_executable(sf4e-ggpo-udp-relay "${SF4E_ROOT}/src/relay/ggpo_udp_relay.cxx")
if(UNIX AND NOT APPLE)
  target_include_directories(sf4e-ggpo-udp-relay SYSTEM BEFORE PUBLIC "${LINUX_SHIM}")
endif()
target_link_libraries(sf4e-ggpo-udp-relay PRIVATE
  CLI11::CLI11
  spdlog::spdlog
)

install(TARGETS sf4e-ggpo-udp-relay RUNTIME DESTINATION bin)
