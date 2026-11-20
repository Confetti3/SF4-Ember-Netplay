# The menu backdrop (src/sf4e/sf4e__MenuBackdrop.hxx). Its rules and the
# movie frames' list are pure, for the game, the launcher and the tests.
add_library(sf4e_menu_backdrop_rules STATIC "src/common/MenuBackdropRules.cxx" "src/common/MenuBackdropRules.hxx"
    "src/common/MovieFrames.cxx" "src/common/MovieFrames.hxx")
target_compile_features(sf4e_menu_backdrop_rules PUBLIC cxx_std_17)
if(BUILD_TESTING)
    sf4e_add_unit_test(NAME MenuBackdrop SOURCES "src/tests/menu_backdrop_test.cxx" LIBS sf4e_menu_backdrop_rules TIMEOUT 30)
endif()
