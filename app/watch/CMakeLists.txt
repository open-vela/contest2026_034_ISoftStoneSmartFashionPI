# Contest 2026 - Boot logo & animation for ESP32-S3 Touch AMOLED

if(CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT)

  # Main source file
  set(MAINSRC watch_main.c)

  # Collect source files from subdirectories
  file(GLOB APPS_CSRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/boot/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/launcher/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/common/*.c
    # 表情UI设置界面已禁用，使用手表UI的设置界面（app_watch/apps/settings/）
    # ${CMAKE_CURRENT_SOURCE_DIR}/apps/settings/*.c
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
  # Exclude MiSans_Regular.c — app_watch provides a larger subset (192KB vs 165KB)
  # that matches the MiSans_Regular_ttf_size declared in app_watch's header
  list(FILTER RESOURCE_CSRCS EXCLUDE REGEX ".*/app/watch/resource/font/generated/MiSans_Regular\\.c$")

  # Expression GIF images are loaded from SD card at runtime,
  # no C array data is compiled into firmware.

  # Boot animation frame data (PNG assets)
  file(GLOB POWER_PNG_ASSETS
    ${CMAKE_CURRENT_SOURCE_DIR}/resource/power_png/generated/*.c
  )

  list(APPEND RESOURCE_CSRCS ${POWER_PNG_ASSETS})

  # ============================================================
  # Vendor watch UI sources (app_watch)
  # ============================================================
  set(VENDOR_WATCH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../app_watch)

  # app_watch apps (tong_ai uses volcengine E2E, independent of ai_agent)
  file(GLOB_RECURSE VW_APPS_CSRCS
    ${VENDOR_WATCH_DIR}/apps/alarm/*.c
    ${VENDOR_WATCH_DIR}/apps/boot/*.c
    ${VENDOR_WATCH_DIR}/apps/calendar/*.c
    ${VENDOR_WATCH_DIR}/apps/common/*.c
    ${VENDOR_WATCH_DIR}/apps/home_control/*.c
    ${VENDOR_WATCH_DIR}/apps/launcher/*.c
    ${VENDOR_WATCH_DIR}/apps/settings/*.c
    ${VENDOR_WATCH_DIR}/apps/sos/*.c
    ${VENDOR_WATCH_DIR}/apps/sports/*.c
    ${VENDOR_WATCH_DIR}/apps/stopwatch/*.c
    ${VENDOR_WATCH_DIR}/apps/weather/*.c
    ${VENDOR_WATCH_DIR}/apps/tong_ai/*.c
  )

  # app_watch resource manager + generated assets
  set(VW_RESOURCE_CSRCS
    ${VENDOR_WATCH_DIR}/resource/resource.c
  )
  # 新增图片资源后需重新 configure（GLOB 无 CONFIGURE_DEPENDS）：
  # 修改本文件（或运行 flash_esp32s3.sh -C）触发 CMake 重新扫描 generated 目录。
  # 生成流程：assert/ 下 PNG → nuttx/tools/convert_lvgl_assets.py → image/generated/*.c
  file(GLOB VW_IMG_ASSETS ${VENDOR_WATCH_DIR}/resource/image/generated/*.c)
  file(GLOB VW_FONT_ASSETS ${VENDOR_WATCH_DIR}/resource/font/generated/*.c)
  # Exclude AlibabaPuHuiTi font (removed to save ~163KB DRAM)
  # MiSans_Regular.c IS included from app_watch — matches the header size
  list(FILTER VW_FONT_ASSETS EXCLUDE REGEX ".*AlibabaPuHuiTi_3_55_Regular\.c$")
  file(GLOB VW_PWR_JPG_ASSETS ${VENDOR_WATCH_DIR}/resource/power_jpg/generated/*.c)
  file(GLOB VW_PWR_PNG_ASSETS ${VENDOR_WATCH_DIR}/resource/power_png/generated/*.c)
  list(APPEND VW_RESOURCE_CSRCS ${VW_IMG_ASSETS} ${VW_FONT_ASSETS} ${VW_PWR_JPG_ASSETS} ${VW_PWR_PNG_ASSETS})

  # app_watch page system + utils + demo entry
  # Exclude files with missing HAL dependencies: heart_rate, _template, dialplate
  file(GLOB VW_PAGE_CSRCS ${VENDOR_WATCH_DIR}/page/*.c)
  list(FILTER VW_PAGE_CSRCS EXCLUDE REGEX ".*heart_rate\\.c$|.*_template\\.c$|.*dialplate\\.c$")
  file(GLOB VW_UTILS_CSRCS ${VENDOR_WATCH_DIR}/utils/*.c)
  set(VW_DEMO_SRC ${VENDOR_WATCH_DIR}/watch_demo.c)

  # Merge all source files
  set(ALL_CSRCS ${APPS_CSRCS} ${RESOURCE_CSRCS} ${VW_APPS_CSRCS} ${VW_RESOURCE_CSRCS} ${VW_PAGE_CSRCS} ${VW_UTILS_CSRCS} ${VW_DEMO_SRC})

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
    jsonc
    curl
    mbedtls
    SRCS
    ${MAINSRC} ${ALL_CSRCS}
  )

  # Add include paths
  target_include_directories(watch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VENDOR_WATCH_DIR}
    ${VENDOR_WATCH_DIR}/resource
    ${VENDOR_WATCH_DIR}/resource/image/generated
    ${VENDOR_WATCH_DIR}/resource/font/generated
    ${VENDOR_WATCH_DIR}/resource/power_jpg/generated
    ${VENDOR_WATCH_DIR}/resource/power_png/generated
    ${VENDOR_WATCH_DIR}/page
    ${VENDOR_WATCH_DIR}/utils
  )

  # Force-include display compatibility header
  target_compile_options(watch PRIVATE
    -include ${VENDOR_WATCH_DIR}/apps/common/display_compat.h
  )

endif()
