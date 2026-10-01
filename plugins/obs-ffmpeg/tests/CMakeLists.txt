add_executable(test-ffmpeg-video-encoder-delay test-video-encoder-delay.c)
target_link_libraries(
  test-ffmpeg-video-encoder-delay
  PRIVATE OBS::libobs OBS::opts-parser FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil
)
add_test(NAME ffmpeg-video-encoder-delay COMMAND test-ffmpeg-video-encoder-delay)

if(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
  add_executable(test-vaapi-device-cache test-vaapi-device-cache.c)
  target_include_directories(test-vaapi-device-cache PRIVATE "${CMAKE_SOURCE_DIR}/shared/obs-drm-helpers")
  target_link_libraries(test-vaapi-device-cache PRIVATE OBS::libobs FFmpeg::avutil Libva::va Libva::drm Libdrm::Libdrm)
  add_test(NAME vaapi-device-cache COMMAND test-vaapi-device-cache)
endif()
