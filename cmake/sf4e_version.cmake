# Embed Windows VERSIONINFO + manifest on shipped binaries (helps SmartScreen / Defender reputation).
# The product's names and icon come from sf4e_product.cmake.
include("${CMAKE_CURRENT_LIST_DIR}/sf4e_product.cmake")

function(sf4e_configure_windows_version target file_description original_filename file_type)
  if(NOT MSVC)
    return()
  endif()

  if(NOT DEFINED PROJECT_VERSION_MAJOR)
    message(FATAL_ERROR "sf4e_configure_windows_version requires project() to have been called first")
  endif()

  set(SF4E_BRAND_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
  set(SF4E_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
  set(SF4E_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
  set(SF4E_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
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
  # The resource compiler's dependency scan follows #include only, not the
  # ICON line, so a new ember.ico would otherwise leave the old icon embedded.
  set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}.rc"
    APPEND PROPERTY OBJECT_DEPENDS "${SF4E_PRODUCT_ICON}")
endfunction()

# Embeds `manifest`, a path the product file names (cmake/sf4e_product.cmake).
function(sf4e_embed_manifest target manifest)
  if(NOT MSVC)
    return()
  endif()
  if(NOT EXISTS "${manifest}")
    message(FATAL_ERROR "Manifest not found: ${manifest}")
  endif()
  # Let CMake merge this with its generated manifest, including incremental
  # Ninja links. /MANIFEST:EMBED would embed it again and duplicate resource 1.
  target_sources("${target}" PRIVATE "${manifest}")
endfunction()
