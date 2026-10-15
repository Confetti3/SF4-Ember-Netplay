# Unpacks an archive whose SHA-256 the caller has already checked, unless an
# earlier unpack of the same archive is still whole: its receipt inside the
# destination names that hash and every listed file is there. The receipt is
# written last, after the files are found, so a partial unpack or a file
# deleted since is unpacked again.
#   sf4e_unpack_verified(<archive> <sha256> <destination> <file...>)
# Each file is a path relative to the destination.
function(sf4e_unpack_verified archive sha256 destination)
    set(receipt "${destination}/unpacked.sha256")
    set(whole FALSE)
    if(EXISTS "${receipt}")
        file(READ "${receipt}" unpacked)
        if(unpacked STREQUAL sha256)
            set(whole TRUE)
            foreach(file IN LISTS ARGN)
                if(NOT EXISTS "${destination}/${file}")
                    set(whole FALSE)
                endif()
            endforeach()
        endif()
    endif()
    if(whole)
        return()
    endif()
    file(REMOVE_RECURSE "${destination}")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${destination}")
    foreach(file IN LISTS ARGN)
        if(NOT EXISTS "${destination}/${file}")
            message(FATAL_ERROR "${archive} has no ${file}")
        endif()
    endforeach()
    file(WRITE "${receipt}" "${sha256}")
endfunction()
