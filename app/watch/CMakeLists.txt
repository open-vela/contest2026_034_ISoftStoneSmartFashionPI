# Contest 2026 - Boot logo & animation for ESP32-S3 Touch AMOLED

if(CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT)

  # Main source file
  set(MAINSRC watch_main.c)

  # Collect source files from subdirectories
  file(GLOB APPS_CSRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/boot/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/launcher/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/common/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/settings/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/home_control/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/volume_control/*.c
  )

  # Resource files (image data)
  file(GLOB RESOURCE_CSRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/resource/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/resource/image/app/boot/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/resource/image/generated/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/resource/font/generated/*.c
  )

  # Expression GIF images are loaded from SD card at runtime,
  # no C array data is compiled into firmware.

  # Boot animation frame data (PNG assets)
  file(GLOB POWER_PNG_ASSETS
    ${CMAKE_CURRENT_SOURCE_DIR}/resource/power_png/generated/*.c
  )

  list(APPEND RESOURCE_CSRCS ${POWER_PNG_ASSETS})

  # Merge all source files
  set(ALL_CSRCS ${APPS_CSRCS} ${RESOURCE_CSRCS})

  nuttx_add_application(
    NAME
    watch
    PRIORITY
    100
    STACKSIZE
    32768
    MODULE
    ${CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT}
    DEPENDS
    lvgl
    SRCS
    ${MAINSRC} ${ALL_CSRCS}
  )

  # Add include path for the watch app root directory
  # so that #include "resource/resource.h" and #include "apps/launcher/launcher.h" work
  target_include_directories(watch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

endif()
