include("${CMAKE_CURRENT_LIST_DIR}/sf4e_embed.cmake")
# Ember's frame data (assets/framedata/usf4.json), embedded in the library that
# compiles training/RemoteServer.cxx. It ships inside Sidecar.dll and is not a file
# of the package: an older updater refuses any package file it does not know
# (Unexpected package file), so a new package path would strand it.
sf4e_hex_bytes("${CMAKE_CURRENT_SOURCE_DIR}/assets/framedata/usf4.json" sf4e_frame_data_bytes)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/FrameDataTable.hxx" CONTENT
    "#pragma once\n// Generated from assets/framedata/usf4.json by cmake/sf4e_frame_data.cmake.\nnamespace sf4e { namespace training {\nstatic const unsigned char frameDataTable[] = {\n${sf4e_frame_data_bytes}\n0x00};\n} }\n")
target_include_directories(sf4e PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
