# Embed Windows VERSIONINFO + manifest on shipped binaries (helps SmartScreen / Defender reputation).

function(sf4e_configure_windows_version target file_description original_filename file_type)
  if(NOT MSVC)
    return()
  endif()

  set(SF4E_VERSION_MAJOR 0)
  set(SF4E_VERSION_MINOR 3)
  set(SF4E_VERSION_PATCH 2)
  set(SF4E_FILE_DESCRIPTION "${file_description}")
  set(SF4E_INTERNAL_NAME "${original_filename}")
  set(SF4E_ORIGINAL_FILENAME "${original_filename}")
  set(SF4E_FILE_TYPE "${file_type}")

  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}.rc"
    @ONLY
  )
  target_sources("${target}" PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}.rc")
endfunction()

function(sf4e_embed_manifest target manifest_filename)
  if(NOT MSVC)
    return()
  endif()
  set(_manifest "${CMAKE_SOURCE_DIR}/cmake/${manifest_filename}")
  if(NOT EXISTS "${_manifest}")
    message(WARNING "Manifest not found: ${_manifest}")
    return()
  endif()
  target_link_options("${target}" PRIVATE
    "/MANIFEST:EMBED"
    "/MANIFESTINPUT:${_manifest}"
  )
endfunction()
