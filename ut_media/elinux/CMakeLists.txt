cmake_minimum_required(VERSION 3.15)
set(PROJECT_NAME "ut_media")
project(${PROJECT_NAME} LANGUAGES CXX)

set(PLUGIN_NAME "ut_media_plugin")

find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSE REQUIRED libpulse libpulse-simple)
pkg_check_modules(OPUS REQUIRED opus)
pkg_check_modules(GLIB REQUIRED glib-2.0 gio-2.0)
pkg_check_modules(VORBISFILE vorbisfile)
pkg_check_modules(OPUSFILE opusfile)

add_library(${PLUGIN_NAME} SHARED
  "ut_media_plugin.cc"
)

apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
if(VORBISFILE_FOUND)
  target_compile_definitions(${PLUGIN_NAME} PRIVATE UT_MEDIA_HAVE_VORBIS)
  target_include_directories(${PLUGIN_NAME} PRIVATE ${VORBISFILE_INCLUDE_DIRS})
  target_link_libraries(${PLUGIN_NAME} PRIVATE ${VORBISFILE_LIBRARIES})
endif()
if(OPUSFILE_FOUND)
  target_compile_definitions(${PLUGIN_NAME} PRIVATE UT_MEDIA_HAVE_OPUSFILE)
  target_include_directories(${PLUGIN_NAME} PRIVATE ${OPUSFILE_INCLUDE_DIRS})
  target_link_libraries(${PLUGIN_NAME} PRIVATE ${OPUSFILE_LIBRARIES})
endif()

target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  ${PULSE_INCLUDE_DIRS}
  ${OPUS_INCLUDE_DIRS}
  ${GLIB_INCLUDE_DIRS}
)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(${PLUGIN_NAME} PRIVATE
  flutter
  flutter_wrapper_plugin
  ${PULSE_LIBRARIES}
  ${OPUS_LIBRARIES}
  ${GLIB_LIBRARIES}
)

set(ut_media_bundled_libraries
  ""
  PARENT_SCOPE
)
