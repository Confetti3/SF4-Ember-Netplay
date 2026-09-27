include("${CMAKE_CURRENT_LIST_DIR}/sf4e_embed.cmake")
# locales/locales.json is the only declaration of a language or a script. From
# it this file generates the Locale and Script enums (public), the locale table
# and the catalogs (private to Localization.cxx); sf4e_fonts.cmake and
# scripts/subset-cjk-fonts.py read the same file for the script fonts.
set(sf4e_locale_source "${CMAKE_CURRENT_SOURCE_DIR}/locales")
set(sf4e_locale_registry "${sf4e_locale_source}/locales.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sf4e_locale_registry}")
file(READ "${sf4e_locale_registry}" sf4e_locale_json)

set(sf4e_locale_ids "")
set(sf4e_locale_rows "")
# Each catalog is a NUL-terminated byte array: translated catalogs outgrow the
# 64 KB MSVC allows a string literal.
set(sf4e_locale_catalogs "")
string(JSON sf4e_locale_count LENGTH "${sf4e_locale_json}" locales)
math(EXPR sf4e_locale_last "${sf4e_locale_count} - 1")
foreach(index RANGE ${sf4e_locale_last})
    string(JSON id GET "${sf4e_locale_json}" locales ${index} id)
    string(JSON tag GET "${sf4e_locale_json}" locales ${index} tag)
    string(JSON name GET "${sf4e_locale_json}" locales ${index} name)
    string(JSON script GET "${sf4e_locale_json}" locales ${index} script)
    string(JSON game_type TYPE "${sf4e_locale_json}" locales ${index} game)
    if(game_type STREQUAL "NULL")
        set(game "nullptr")
    else()
        string(JSON game GET "${sf4e_locale_json}" locales ${index} game)
        set(game "\"${game}\"")
    endif()
    sf4e_hex_bytes("${sf4e_locale_source}/${tag}.po" catalog_bytes)
    string(APPEND sf4e_locale_catalogs "static const unsigned char ${id}[] = {\n${catalog_bytes}\n0x00};\n")
    string(APPEND sf4e_locale_ids "${id}, ")
    string(APPEND sf4e_locale_rows "    {\"${tag}\", \"${name}\", embedded::${id}, Script::${script}, ${game}},\n")
endforeach()

set(sf4e_script_ids "")
string(JSON sf4e_script_count LENGTH "${sf4e_locale_json}" scripts)
math(EXPR sf4e_script_last "${sf4e_script_count} - 1")
foreach(index RANGE ${sf4e_script_last})
    string(JSON id GET "${sf4e_locale_json}" scripts ${index} id)
    string(APPEND sf4e_script_ids "${id}, ")
endforeach()

set(sf4e_locale_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(sf4e_locale_notice "// Generated from locales/locales.json by cmake/sf4e_locales.cmake.\n")
file(GENERATE OUTPUT "${sf4e_locale_generated}/public/LocaleIds.hxx" CONTENT
    "#pragma once\n${sf4e_locale_notice}namespace sf4e { namespace loc {\n// En leads: it is the fallback catalog.\nenum class Locale { ${sf4e_locale_ids}Count };\n// The writing system a locale needs from the UI fonts.\nenum class Script { ${sf4e_script_ids}};\n} }\n")
file(GENERATE OUTPUT "${sf4e_locale_generated}/EmbeddedLocales.hxx" CONTENT
    "#pragma once\n${sf4e_locale_notice}namespace sf4e { namespace loc { namespace embedded {\n${sf4e_locale_catalogs}} } }\n")
# The rows of the locale table, included inside its initializer.
file(GENERATE OUTPUT "${sf4e_locale_generated}/LocaleTable.inc" CONTENT "${sf4e_locale_notice}${sf4e_locale_rows}")
