# sf4e_unpack_verified (cmake/sf4e_verified_unpack.cmake) over a stand-in for
# the Discord SDK holding the files the build takes from it: an unchanged whole
# unpack is kept, and a missing file (cdiscord.h, which only discordpp.h
# names) or another archive's receipt unpacks again. Run with
# cmake -DSOURCE_DIR=<repository> -DWORK=<empty folder> -P.
include("${SOURCE_DIR}/cmake/sf4e_verified_unpack.cmake")
include("${SOURCE_DIR}/cmake/sf4e_discord_sdk_files.cmake")
function(expect_file path present message)
    if(EXISTS "${path}")
        set(found TRUE)
    else()
        set(found FALSE)
    endif()
    if(NOT found STREQUAL present)
        message(FATAL_ERROR "Verified unpack: ${message}")
    endif()
endfunction()

file(REMOVE_RECURSE "${WORK}")
foreach(file IN LISTS SF4E_DISCORD_SDK_FILES)
    file(WRITE "${WORK}/content/${file}" "${file}")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${WORK}/archive.zip" --format=zip discord_social_sdk
    WORKING_DIRECTORY "${WORK}/content" RESULT_VARIABLE packed)
if(NOT packed EQUAL 0)
    message(FATAL_ERROR "Verified unpack: the fixture archive was not made")
endif()
file(SHA256 "${WORK}/archive.zip" sha256)
set(out "${WORK}/out")
set(header "${out}/discord_social_sdk/include/cdiscord.h")

sf4e_unpack_verified("${WORK}/archive.zip" "${sha256}" "${out}" ${SF4E_DISCORD_SDK_FILES})
foreach(file IN LISTS SF4E_DISCORD_SDK_FILES)
    expect_file("${out}/${file}" TRUE "the first unpack is missing ${file}")
endforeach()

# A file the archive does not hold survives only while the unpack is kept.
file(WRITE "${out}/kept.txt" "")
sf4e_unpack_verified("${WORK}/archive.zip" "${sha256}" "${out}" ${SF4E_DISCORD_SDK_FILES})
expect_file("${out}/kept.txt" TRUE "a whole unpack of the same archive was unpacked again")

file(REMOVE "${header}")
sf4e_unpack_verified("${WORK}/archive.zip" "${sha256}" "${out}" ${SF4E_DISCORD_SDK_FILES})
expect_file("${header}" TRUE "a deleted cdiscord.h was not unpacked again")
expect_file("${out}/kept.txt" FALSE "a partial unpack was kept")

file(WRITE "${out}/kept.txt" "")
file(WRITE "${out}/unpacked.sha256" "another archive")
sf4e_unpack_verified("${WORK}/archive.zip" "${sha256}" "${out}" ${SF4E_DISCORD_SDK_FILES})
expect_file("${out}/kept.txt" FALSE "another archive's unpack was kept")

file(REMOVE_RECURSE "${WORK}")
message("Verified unpack passed")
