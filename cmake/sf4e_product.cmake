include_guard(GLOBAL)
# SF4 Ember, the product: its identity (src/product/Sf4Product.cxx, read
# through ember::Product()) and what its version resources and header mark are
# built from. Another product supplies its own file in place of this one.
include("${CMAKE_CURRENT_LIST_DIR}/sf4e_embed.cmake")
get_filename_component(SF4E_PRODUCT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(SF4E_PRODUCT_NAME "SF4 Ember Netplay")
set(SF4E_PRODUCT_COMPANY "SF4 Ember Netplay Contributors")
set(SF4E_PRODUCT_COPYRIGHT "MIT License. Unofficial USF4 netplay port.")
set(SF4E_PRODUCT_ICON "${SF4E_PRODUCT_SOURCE_DIR}/src/ui/ember.ico")
set(SF4E_PRODUCT_MARK "${SF4E_PRODUCT_SOURCE_DIR}/src/ui/ember.rgba")
# The application manifests the launcher and Sidecar embed (sf4e_embed_manifest):
# their assembly names and descriptions are the product's.
set(SF4E_PRODUCT_LAUNCHER_MANIFEST "${SF4E_PRODUCT_SOURCE_DIR}/cmake/Launcher.exe.manifest")
set(SF4E_PRODUCT_SIDECAR_MANIFEST "${SF4E_PRODUCT_SOURCE_DIR}/cmake/Sidecar.dll.manifest")

# Compiles the identity into `target`. The approved ink mark is embedded with
# it and packed into the font atlas (Theme.cxx), so no DX9 resource or runtime
# image is needed, including after a device reset.
function(sf4e_add_product_identity target)
    sf4e_embed_brand_mark(${target} "${SF4E_PRODUCT_MARK}")
    target_sources(${target} PRIVATE "${SF4E_PRODUCT_SOURCE_DIR}/src/product/Sf4Product.cxx")
endfunction()
