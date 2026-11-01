include("${CMAKE_CURRENT_LIST_DIR}/sf4e_embed.cmake")
# The chat filter's word lists (assets/chat-filter/README.md): each LDNOOBW
# language list as it was downloaded, Ember's allow list of everyday words
# taken back out of them, and Ember's stems matched inside words. Embedded,
# so the filter never reads loose files. Each entry is matched by what it is
# written in (ChatFilter.cxx), not by the list it comes from.
set(sf4e_chat_filter_source "${CMAKE_CURRENT_SOURCE_DIR}/assets/chat-filter")
set(sf4e_chat_filter_arrays "")
set(sf4e_chat_filter_ids "")
foreach(tag cs de en es fr it ja ko nl pl pt ru zh)
    string(MAKE_C_IDENTIFIER "list_${tag}" id)
    sf4e_hex_bytes("${sf4e_chat_filter_source}/${tag}.txt" bytes)
    string(APPEND sf4e_chat_filter_arrays "static const unsigned char ${id}[] = {\n${bytes}\n0x00};\n")
    string(APPEND sf4e_chat_filter_ids "${id}, ")
endforeach()
sf4e_hex_bytes("${sf4e_chat_filter_source}/allow.txt" sf4e_chat_filter_allow)
sf4e_hex_bytes("${sf4e_chat_filter_source}/stems.txt" sf4e_chat_filter_stems)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/ChatFilterLists.hxx" CONTENT
    "#pragma once\n// Generated from assets/chat-filter by cmake/sf4e_chat_filter.cmake.\nnamespace sf4e { namespace ui { namespace chat_filter {\n${sf4e_chat_filter_arrays}static const unsigned char allow[] = {\n${sf4e_chat_filter_allow}\n0x00};\nstatic const unsigned char stems[] = {\n${sf4e_chat_filter_stems}\n0x00};\nstatic const unsigned char* const lists[] = {${sf4e_chat_filter_ids}};\n} } }\n")
target_include_directories(sf4e_ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
