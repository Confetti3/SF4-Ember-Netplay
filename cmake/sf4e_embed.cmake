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
