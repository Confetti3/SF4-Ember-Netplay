# Script mode, run on every build: writes BuildIdentity.hxx with the source
# revision the binaries were built from, so a field launcher.log or sf4e.log
# names its build. The header changes only when the revision does.
#   cmake -DSOURCE_DIR=<repo> -DOUTPUT=<header> -P sf4e_build_identity.cmake
execute_process(COMMAND git -C "${SOURCE_DIR}" rev-parse --short=12 HEAD
    OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE failed ERROR_QUIET)
if(failed OR NOT revision)
    set(revision "unknown")
else()
    execute_process(COMMAND git -C "${SOURCE_DIR}" status --porcelain --untracked-files=no
        OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(changes)
        string(APPEND revision "-dirty")
    endif()
endif()
file(WRITE "${OUTPUT}.next" "#pragma once\n#define SF4E_SOURCE_REVISION \"${revision}\"\n")
configure_file("${OUTPUT}.next" "${OUTPUT}" COPYONLY)
file(REMOVE "${OUTPUT}.next")
