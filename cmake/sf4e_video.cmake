# Ember's own video export, split where its two processes part: the game's
# side (the frame grab and its end of the link, platform/FrameGrab.hxx and
# VideoLink.hxx) and the encoder's (VideoServe.hxx, VideoEncoder.hxx), which
# runs in a Launcher.exe the game starts for it. Only the encoder's side
# links Media Foundation and the audio device API, delay-loaded: N editions
# without the Media Feature Pack and Wine may lack them, and the launcher
# must still load there.
# The grab's two pixel shaders are compiled here, so the game needs no shader compiler.
find_program(SF4E_FXC fxc REQUIRED)
set(_SF4E_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
file(MAKE_DIRECTORY "${_SF4E_SHADER_DIR}")
foreach(_pass Luma Chroma)
    add_custom_command(
        OUTPUT "${_SF4E_SHADER_DIR}/FrameGrab${_pass}.h"
        COMMAND "${SF4E_FXC}" /nologo /T ps_2_0 /E ${_pass} /Vn kFrameGrab${_pass} /Fh "${_SF4E_SHADER_DIR}/FrameGrab${_pass}.h" "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/FrameGrab.hlsl"
        DEPENDS "src/platform/FrameGrab.hlsl"
        VERBATIM
    )
endforeach()
add_library(sf4e_video_capture STATIC "src/platform/FrameGrab.cxx" "src/platform/FrameGrab.hxx" "src/platform/VideoLink.cxx" "src/platform/VideoLink.hxx" "src/platform/VideoShared.hxx"
    "${_SF4E_SHADER_DIR}/FrameGrabLuma.h" "${_SF4E_SHADER_DIR}/FrameGrabChroma.h")
target_compile_features(sf4e_video_capture PUBLIC cxx_std_17)
target_include_directories(sf4e_video_capture PRIVATE "${_SF4E_SHADER_DIR}")
target_link_libraries(sf4e_video_capture PUBLIC spdlog::spdlog)
add_library(sf4e_video_encoder STATIC "src/platform/VideoEncoder.cxx" "src/platform/VideoEncoder.hxx" "src/platform/VideoServe.cxx" "src/platform/VideoServe.hxx" "src/platform/VideoShared.hxx")
target_compile_features(sf4e_video_encoder PUBLIC cxx_std_17)
target_link_libraries(sf4e_video_encoder PUBLIC spdlog::spdlog mfplat mfreadwrite mfuuid mmdevapi d3d11 dxgi ole32 delayimp)
target_link_options(sf4e_video_encoder INTERFACE "/DELAYLOAD:mfplat.dll" "/DELAYLOAD:mfreadwrite.dll" "/DELAYLOAD:MMDevAPI.dll" "/DELAYLOAD:d3d11.dll" "/DELAYLOAD:dxgi.dll")
if(BUILD_TESTING)
    add_executable(VideoEncoderTest "src/tests/video_encoder_test.cxx")
    target_link_libraries(VideoEncoderTest PRIVATE sf4e_video_capture sf4e_video_encoder winmm)
    add_test(NAME VideoEncoder COMMAND VideoEncoderTest)
    set_tests_properties(VideoEncoder PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
    add_executable(FrameGrabTest "src/tests/frame_grab_test.cxx")
    target_link_libraries(FrameGrabTest PRIVATE sf4e_video_capture d3d9)
    add_test(NAME FrameGrab COMMAND FrameGrabTest)
    set_tests_properties(FrameGrab PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
