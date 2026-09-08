# ESP-IDFプロジェクトのルート。
# ターゲットはESP32-S3 (Waveshare ESP32-S3-Touch-LCD-7B: ESP32-S3-WROOM-1-N16R8)。
# 設定はsdkconfig.defaultsに置く。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(schedule-viewer-esp32s3)

# 日本語TTF(fonts/MPLUS1-ExtraBold.ttf)をfontパーティションへ書き込む。
# SDカードが無い基板なのでフォントはフラッシュに生のまま置き、
# font_ttf.cppがesp_partition_mmapで直接読む。idf.py flashとbuild/flash_args
# の両方に自動で含まれる。
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/fonts/MPLUS1-ExtraBold.ttf ${CMAKE_BINARY_DIR}/font.bin COPYONLY)
esptool_py_flash_to_partition(flash "font" "${CMAKE_BINARY_DIR}/font.bin")
