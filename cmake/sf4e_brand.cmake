# Embed the approved ink mark in the existing font atlas. No extra DX9 resource
# or runtime image dependency is needed, including after a device reset.
set(sf4e_brand_path "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/ember.rgba")
file(SIZE "${sf4e_brand_path}" sf4e_brand_size)
if(NOT sf4e_brand_size EQUAL 65536)
    message(FATAL_ERROR "Ember atlas must be 128x128 RGBA; run scripts/build-ember-brand.py")
endif()
sf4e_hex_bytes("${sf4e_brand_path}" sf4e_brand_bytes)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedBrand.hxx" CONTENT
    "#pragma once\nnamespace sf4e { namespace ui { namespace brand {\nconstexpr int Size = 128;\nstatic const unsigned char Pixels[] = {\n${sf4e_brand_bytes}\n};\n} } }\n")

# BluMint's wordmark, for the rooms of BluMint's matches (used with BluMint's
# permission): 256x64 RGBA, rendered from blumint.com/blumint-wordmark.svg.
set(sf4e_blumint_path "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/blumint.rgba")
file(SIZE "${sf4e_blumint_path}" sf4e_blumint_size)
if(NOT sf4e_blumint_size EQUAL 65536)
    message(FATAL_ERROR "BluMint's wordmark must be 256x64 RGBA")
endif()
sf4e_hex_bytes("${sf4e_blumint_path}" sf4e_blumint_bytes)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedBluMint.hxx" CONTENT
    "#pragma once\nnamespace sf4e { namespace ui { namespace blumint {\nconstexpr int Width = 256, Height = 64;\nstatic const unsigned char Pixels[] = {\n${sf4e_blumint_bytes}\n};\n} } }\n")
