# Embed original font bytes; running the mod never needs loose fonts or downloads.
set(sf4e_font_source "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/fonts")
set(sf4e_font_header "#pragma once\n#include \"LocaleIds.hxx\"\nnamespace sf4e { namespace ui { namespace fonts {\n")
# sf4e_hex_bytes is also used by sf4e_brand.cmake, which is included after this file.
include("${CMAKE_CURRENT_LIST_DIR}/sf4e_embed.cmake")
function(sf4e_embed_font filename symbol expected_hash)
    set(font_path "${sf4e_font_source}/${filename}")
    file(SHA256 "${font_path}" font_hash)
    if(NOT font_hash STREQUAL expected_hash)
        message(FATAL_ERROR "Unexpected font content: ${filename}")
    endif()
    sf4e_hex_bytes("${font_path}" font_bytes)
    set(sf4e_font_header "${sf4e_font_header}alignas(4) static const unsigned char ${symbol}[] = {\n${font_bytes}\n};\n" PARENT_SCOPE)
endfunction()
sf4e_embed_font("Inter-Regular.ttf" Body "40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82")
sf4e_embed_font("Inter-SemiBold.ttf" Heading "78a843fade9d4612a5567302fb595b56976eb5fcebf4fea5a5912d638bafcde3")
# Scripts Inter lacks name a font in locales/locales.json (read by
# sf4e_locales.cmake). scripts/subset-cjk-fonts.py writes those fonts from the
# catalogs, so their bytes change with the text; NotoSansCJK-Subset.json
# records their source, and the Localization test proves they cover every catalog.
set(sf4e_script_fonts "")
string(JSON sf4e_script_count LENGTH "${sf4e_locale_json}" scripts)
math(EXPR sf4e_script_last "${sf4e_script_count} - 1")
foreach(index RANGE ${sf4e_script_last})
    string(JSON script GET "${sf4e_locale_json}" scripts ${index} id)
    string(JSON font_type TYPE "${sf4e_locale_json}" scripts ${index} font)
    if(NOT font_type STREQUAL "NULL")
        string(JSON font GET "${sf4e_locale_json}" scripts ${index} font)
        sf4e_hex_bytes("${sf4e_font_source}/${font}" font_bytes)
        string(APPEND sf4e_font_header "alignas(4) static const unsigned char ${script}[] = {\n${font_bytes}\n};\n")
        string(APPEND sf4e_script_fonts "    {loc::Script::${script}, ${script}, sizeof(${script})},\n")
    endif()
endforeach()
string(APPEND sf4e_font_header "// The font that draws each script Inter lacks, generated from locales/locales.json.\n"
    "struct ScriptFont { loc::Script script; const unsigned char* data; unsigned bytes; };\n"
    "static const ScriptFont ScriptFonts[] = {\n${sf4e_script_fonts}};\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sf4e_font_source}/OFL.txt")
file(READ "${sf4e_font_source}/OFL.txt" sf4e_font_license)
# The full Noto license ships in notices/; the embedded text names it.
string(APPEND sf4e_font_license "\nNoto Sans CJK (Japanese, Korean and Chinese text): Copyright © 2014-2021 Adobe "
    "(http://www.adobe.com/), under the same SIL Open Font License 1.1.\n")
string(APPEND sf4e_font_header "static const char License[] = R\"InterOFL(${sf4e_font_license})InterOFL\";\n} } }\n")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedFonts.hxx" CONTENT "${sf4e_font_header}")
target_include_directories(sf4e_ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
