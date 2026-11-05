include_guard(GLOBAL)
# Bytes of a file as a C array initializer. Byte arrays have no length limit,
# unlike MSVC string literals, which stop at 64 KB.
function(sf4e_hex_bytes path out_var)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(READ "${path}" hex HEX)
    # Sixteen bytes per line keeps generated compiler input manageable.
    string(REGEX REPLACE "(................................)" "\\1\n" rows "${hex}")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${rows}")
    set(${out_var} "${bytes}" PARENT_SCOPE)
endfunction()

# A product's header mark, `mark` (128x128 RGBA), as the generated
# EmbeddedBrand.hxx that `target` compiles its identity with (Branding::mark).
# Every product embeds its mark this way: SF4 Ember's sf4e_product.cmake, and
# a product that builds on the library from its own repository.
function(sf4e_embed_brand_mark target mark)
    file(SIZE "${mark}" mark_size)
    if(NOT mark_size EQUAL 65536)
        message(FATAL_ERROR "Ember atlas must be 128x128 RGBA; run scripts/build-ember-brand.py")
    endif()
    sf4e_hex_bytes("${mark}" mark_bytes)
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated/product")
    file(GENERATE OUTPUT "${generated}/EmbeddedBrand.hxx" CONTENT
        "#pragma once\nnamespace sf4e { namespace ui { namespace brand {\nconstexpr int Size = 128;\nstatic const unsigned char Pixels[] = {\n${mark_bytes}\n};\n} } }\n")
    target_include_directories(${target} PRIVATE "${generated}")
endfunction()
