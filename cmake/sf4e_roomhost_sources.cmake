# The public room host's C++ sources, read from server/roomhost/sources.txt, the
# one list that server/roomhost/build-linux.sh (g++ alone, no CMake) reads too.
# Each "# [group]" line of the file opens a group; this sets
# SF4E_ROOMHOST_<group> to its paths, in file order. CMakeLists.txt builds
# sf4e_room_model, sf4e_selection, sf4e_common, sf4e_session_server,
# sf4e_iroh_room and sf4e-room-host from them.
# A mistake stops the configure: a path before the first group, a group the
# file's header does not list, a path that is no file, or a group left empty.
set(_sf4e_roomhost_list "${CMAKE_SOURCE_DIR}/server/roomhost/sources.txt")
set(_sf4e_roomhost_groups host host_posix host_windows session_server iroh_room room_model selection common)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sf4e_roomhost_list}")
file(STRINGS "${_sf4e_roomhost_list}" _sf4e_roomhost_lines)
foreach(_group IN LISTS _sf4e_roomhost_groups)
    set(SF4E_ROOMHOST_${_group} "")
endforeach()
set(_sf4e_roomhost_group "")
foreach(_line IN LISTS _sf4e_roomhost_lines)
    # file(STRINGS) drops a CRLF checkout's carriage returns; strip also guards a stray blank.
    string(STRIP "${_line}" _line)
    if(_line MATCHES "^# \\[([a-z_]+)\\]$")
        set(_sf4e_roomhost_group "${CMAKE_MATCH_1}")
        if(NOT _sf4e_roomhost_group IN_LIST _sf4e_roomhost_groups)
            message(FATAL_ERROR "server/roomhost/sources.txt: '${_sf4e_roomhost_group}' is not one of ${_sf4e_roomhost_groups}")
        endif()
    elseif(_line AND NOT _line MATCHES "^#")
        if(NOT _sf4e_roomhost_group)
            message(FATAL_ERROR "server/roomhost/sources.txt: '${_line}' comes before any '# [group]' line")
        endif()
        if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${_line}" OR IS_DIRECTORY "${CMAKE_SOURCE_DIR}/${_line}")
            message(FATAL_ERROR "server/roomhost/sources.txt: no source file '${_line}'")
        endif()
        list(APPEND SF4E_ROOMHOST_${_sf4e_roomhost_group} "${_line}")
    endif()
endforeach()
foreach(_group IN LISTS _sf4e_roomhost_groups)
    if(NOT SF4E_ROOMHOST_${_group})
        message(FATAL_ERROR "server/roomhost/sources.txt: the group '${_group}' lists no source")
    endif()
endforeach()
unset(_group)
unset(_line)
