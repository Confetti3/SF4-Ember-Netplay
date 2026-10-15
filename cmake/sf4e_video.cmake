# Ember's own video export, in two parts. In the game: the frame grab
# (platform/FrameGrab.hxx), the game's end of the link to the encoder
# (VideoLink.hxx), and the capture that drives both (sf4e__ReplayCapture.hxx). In a Launcher.exe the game starts for it: the encoder
# (VideoEncoder.hxx) and the link's other end (VideoLinkServe.hxx). Only the
# encoder takes Media Foundation and the audio device API, delay-loaded: N
# editions without the Media Feature Pack and Wine may lack them, and the
# launcher must still load there.
add_library(sf4e_video_capture STATIC "src/sf4e/sf4e__ReplayCapture.cxx" "src/sf4e/sf4e__ReplayCapture.hxx" "src/platform/FrameGrab.cxx" "src/platform/FrameGrab.hxx" "src/platform/VideoLink.cxx" "src/platform/VideoLink.hxx" "src/platform/VideoLinkProtocol.hxx" "src/platform/VideoTemporary.hxx")
target_compile_features(sf4e_video_capture PUBLIC cxx_std_17)
# The install resolver (VideoLink.cxx) and the temporary file's name (ole32).
target_link_libraries(sf4e_video_capture PUBLIC spdlog::spdlog PRIVATE sf4e_common pathcch shlwapi ole32)
add_library(sf4e_video_encoder STATIC "src/platform/VideoEncoder.cxx" "src/platform/VideoEncoder.hxx" "src/platform/VideoLinkServe.cxx" "src/platform/VideoLinkServe.hxx" "src/platform/VideoLinkProtocol.hxx")
target_compile_features(sf4e_video_encoder PUBLIC cxx_std_17)
target_link_libraries(sf4e_video_encoder PUBLIC spdlog::spdlog mfplat mfreadwrite mfuuid mmdevapi d3d11 dxgi ole32 delayimp)
# The settings root, for the encoder's log (VideoLinkServe.cxx).
target_link_libraries(sf4e_video_encoder PRIVATE sf4e_settings)
target_link_options(sf4e_video_encoder INTERFACE "/DELAYLOAD:mfplat.dll" "/DELAYLOAD:mfreadwrite.dll" "/DELAYLOAD:MMDevAPI.dll" "/DELAYLOAD:d3d11.dll" "/DELAYLOAD:dxgi.dll")
if(BUILD_TESTING)
    # Both ends: it starts itself as the encoder's process, and as a game
    # that cancels an export through the capture (a Direct3D 9 device).
    sf4e_add_unit_test(NAME VideoEncoder SOURCES "src/tests/video_encoder_test.cxx"
        LIBS sf4e_video_capture sf4e_video_encoder winmm d3d9 TIMEOUT 120 SKIP_RETURN_CODE 77)
    sf4e_add_unit_test(NAME FrameGrab SOURCES "src/tests/frame_grab_test.cxx" LIBS sf4e_video_capture d3d9 TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
