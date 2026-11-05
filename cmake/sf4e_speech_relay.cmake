include("${CMAKE_CURRENT_LIST_DIR}/sf4e_embed.cmake")
# The speech relay script (src/platform/speech_relay.py), embedded in the
# library that compiles Speech.cxx. It ships inside the binary and is not a
# file of the package: an older updater refuses any package file it does not
# know (Unexpected package file), so a new package path would strand it.
# Speech.cxx writes it to the prefix's temporary folder when it starts the relay.
sf4e_hex_bytes("${CMAKE_CURRENT_LIST_DIR}/../src/platform/speech_relay.py" sf4e_speech_relay_bytes)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/SpeechRelayScript.hxx" CONTENT
    "#pragma once\n// Generated from src/platform/speech_relay.py by cmake/sf4e_speech_relay.cmake.\nnamespace sf4e { namespace speech {\nstatic const unsigned char relayScript[] = {\n${sf4e_speech_relay_bytes}\n0x00};\n} }\n")
