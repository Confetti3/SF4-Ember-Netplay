include_guard(GLOBAL)
file(STRINGS "${CMAKE_SOURCE_DIR}/rust/sf4-net/rust-toolchain.toml" channel_line
    REGEX "^[ \t]*channel[ \t]*=")
if(NOT channel_line MATCHES "channel[ \t]*=[ \t]*\"([^\"]+)\"")
    message(FATAL_ERROR "Missing Rust toolchain pin")
endif()
set(SF4E_RUST_CHANNEL "${CMAKE_MATCH_1}")
