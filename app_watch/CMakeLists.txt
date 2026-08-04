# ##############################################################################
# vendor/watch/CMakeLists.txt
# ##############################################################################
  
if(CONFIG_LVX_USE_ESP32S3_WATCH)
   
   # 主程序源文件
   set(MAINSRC watch_main.c)
   
   # 收集apps目录下的源文件(排除calendar和settings)
   file(GLOB_RECURSE APPS_CSRCS 
     ${CMAKE_CURRENT_SOURCE_DIR}/apps/common/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/apps/boot/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/apps/launcher/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/apps/settings/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/apps/calendar/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/apps/home_control/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/apps/stopwatch/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/apps/alarm/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/apps/sports/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/apps/sos/*.c
     ${CMAKE_CURRENT_SOURCE_DIR}/apps/weather/*.c
   )
   
   # 收集resource目录下的源文件(排除image目录下的.c文件)
   file(GLOB_RECURSE RESOURCE_CSRCS 
     ${CMAKE_CURRENT_SOURCE_DIR}/resource/*.c
   )
   list(FILTER RESOURCE_CSRCS EXCLUDE REGEX ".*resource/image/.*\\.c$")

   # 显式添加开机动画生成的 C 文件（避免 CMake GLOB 缓存导致新文件未被编译）
   file(GLOB POWER_JPG_ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/resource/power_jpg/generated/*.c)
   file(GLOB POWER_PNG_ASSETS ${CMAKE_CURRENT_SOURCE_DIR}/resource/power_png/generated/*.c)
   list(APPEND RESOURCE_CSRCS ${POWER_JPG_ASSETS} ${POWER_PNG_ASSETS})
   
   # 合并所有源文件
   set(ALL_CSRCS ${APPS_CSRCS} ${RESOURCE_CSRCS})
   
    nuttx_add_application(
      NAME
      watch
      PRIORITY
      100
      STACKSIZE
      32768
      MODULE
      ${CONFIG_LVX_USE_ESP32S3_WATCH}
      DEPENDS
      lvgl
      jsonc
      curl
      SRCS
     ${MAINSRC} ${ALL_CSRCS}
   )
endif()