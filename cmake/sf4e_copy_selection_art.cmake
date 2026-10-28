# Copies the selection art the package ships into the build folder, a file at a
# time and only when its content differs, so a build compares what the product
# reads and writes only what changed. The filter is the install rule's in
# CMakeLists.txt (images, json and md, without the masking photographs
# color-N.png and color-N.jpg, which the runtime does not read): a launch from
# the build folder sees the files a package has.
#   cmake -DSOURCE=<assets/selection> -DDEST=<folder> -P sf4e_copy_selection_art.cmake
if(NOT IS_DIRECTORY "${SOURCE}" OR "${DEST}" STREQUAL "")
    message(FATAL_ERROR "sf4e_copy_selection_art: needs SOURCE, a folder, and DEST")
endif()
file(GLOB_RECURSE files RELATIVE "${SOURCE}" "${SOURCE}/*")
foreach(file IN LISTS files)
    if(file MATCHES "\.(png|jpg|json|md)$" AND NOT file MATCHES "(^|/)color-[0-9]+\.(png|jpg)$")
        get_filename_component(folder "${DEST}/${file}" DIRECTORY)
        file(MAKE_DIRECTORY "${folder}")
        file(COPY_FILE "${SOURCE}/${file}" "${DEST}/${file}" ONLY_IF_DIFFERENT RESULT result)
        if(NOT result STREQUAL "0")
            message(FATAL_ERROR "sf4e_copy_selection_art: ${file}: ${result}")
        endif()
    endif()
endforeach()
