# The game's replay files: archiving and importing (platform/ReplayFiles.hxx).
# The portable replay tests are registered with the core tests, since the
# core-only build stops before this library and its spdlog.
add_library(sf4e_replay_files STATIC "src/platform/ReplayFiles.cxx" "src/platform/ReplayFiles.hxx" "src/common/ReplaySlots.hxx")
target_compile_features(sf4e_replay_files PUBLIC cxx_std_17)
target_link_libraries(sf4e_replay_files PUBLIC spdlog::spdlog PRIVATE sf4e_durable_file shell32 ole32 bcrypt)
