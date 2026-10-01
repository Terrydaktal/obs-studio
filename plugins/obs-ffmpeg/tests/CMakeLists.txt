add_executable(test-ffmpeg-video-encoder-delay test-video-encoder-delay.c)
target_link_libraries(
  test-ffmpeg-video-encoder-delay
  PRIVATE OBS::libobs OBS::opts-parser FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil
)
add_test(NAME ffmpeg-video-encoder-delay COMMAND test-ffmpeg-video-encoder-delay)
