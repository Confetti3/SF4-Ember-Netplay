# Embed the approved ink mark in the existing font atlas. No extra DX9 resource
# or runtime image dependency is needed, including after a device reset.
set(sf4e_brand_path "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ember.rgba")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sf4e_brand_path}")
file(SIZE "${sf4e_brand_path}" sf4e_brand_size)
if(NOT sf4e_brand_size EQUAL 65536)
    message(FATAL_ERROR "Ember atlas must be 128x128 RGBA; run scripts/build-ember-brand.py")
endif()
file(READ "${sf4e_brand_path}" sf4e_brand_hex HEX)
string(REGEX REPLACE "(................................)" "\\1\n" sf4e_brand_rows "${sf4e_brand_hex}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," sf4e_brand_bytes "${sf4e_brand_rows}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedBrand.hxx" CONTENT
    "#pragma once\nnamespace sf4e { namespace ui { namespace brand {\nconstexpr int Size = 128;\nstatic const unsigned char Pixels[] = {\n${sf4e_brand_bytes}\n};\n} } }\n")
