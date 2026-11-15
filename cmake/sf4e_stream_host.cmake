include("${CMAKE_CURRENT_LIST_DIR}/sf4e_embed.cmake")
# The stream PC's control panel (scripts/stream-host), embedded in Launcher.exe.
# It ships inside the binary and is not a file of the package: an older updater
# refuses any package file it does not know (Unexpected package file), so a new
# package path would strand it. Launcher.exe --write-stream-host writes the files
# to %APPDATA%\sf4e\stream-host (launcher/StreamHostFiles.cxx).
sf4e_hex_bytes("${CMAKE_CURRENT_SOURCE_DIR}/scripts/stream-host/EmberStreamHost.cmd" sf4e_stream_host_cmd_bytes)
sf4e_hex_bytes("${CMAKE_CURRENT_SOURCE_DIR}/scripts/stream-host/EmberStreamHost.ps1" sf4e_stream_host_ps1_bytes)
sf4e_hex_bytes("${CMAKE_CURRENT_SOURCE_DIR}/scripts/stream-host/README.md" sf4e_stream_host_readme_bytes)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/StreamHostScripts.hxx" CONTENT
    "#pragma once\n// Generated from scripts/stream-host by cmake/sf4e_stream_host.cmake.\nnamespace sf4e { namespace launcher {\nstatic const unsigned char streamHostCmd[] = {\n${sf4e_stream_host_cmd_bytes}\n0x00};\nstatic const unsigned char streamHostScript[] = {\n${sf4e_stream_host_ps1_bytes}\n0x00};\nstatic const unsigned char streamHostReadme[] = {\n${sf4e_stream_host_readme_bytes}\n0x00};\n} }\n")
target_include_directories(Launcher PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
