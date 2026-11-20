# The menu backdrop (src/sf4e/sf4e__MenuBackdrop.hxx). Its rules and the
# movie frames' list are pure, for the game, the launcher and the tests. The
# launcher's frame decoder (src/platform/MovieFrameService.hxx) is the only
# part that takes Media Foundation, delay-loaded as the video export's
# encoder is: N editions without the Media Feature Pack and Wine may lack
# it, and the launcher must still load there. Nothing here decodes video in
# the game.
add_library(sf4e_menu_backdrop_rules STATIC "src/common/MenuBackdropRules.cxx" "src/common/MenuBackdropRules.hxx"
    "src/common/MovieFrames.cxx" "src/common/MovieFrames.hxx")
target_compile_features(sf4e_menu_backdrop_rules PUBLIC cxx_std_17)
add_library(sf4e_movie_frame_service STATIC "src/platform/MovieFrameService.cxx" "src/platform/MovieFrameService.hxx"
    "src/platform/MovieFrameMailbox.hxx")
target_compile_features(sf4e_movie_frame_service PUBLIC cxx_std_17)
target_link_libraries(sf4e_movie_frame_service PUBLIC sf4e_menu_backdrop_rules
    PRIVATE mfplat mfreadwrite mfuuid windowscodecs ole32 oleaut32 shell32 delayimp)
target_link_options(sf4e_movie_frame_service INTERFACE "/DELAYLOAD:mfplat.dll" "/DELAYLOAD:mfreadwrite.dll")
if(BUILD_TESTING)
    sf4e_add_unit_test(NAME MenuBackdrop SOURCES "src/tests/menu_backdrop_test.cxx" LIBS sf4e_menu_backdrop_rules TIMEOUT 30)
    # The mailbox both ends, and a decode of a movie when the game's folder
    # is known (SF4E_GAME_DIR); skipped without it.
    sf4e_add_unit_test(NAME MovieFrameService SOURCES "src/tests/movie_frame_service_test.cxx" LIBS sf4e_movie_frame_service
        TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
