# 开机Logo与动画移植说明

## 概述

将 OpenVela 参考代码库 (`/home/ubuntu/openvela/vendor/watch/`) 中的开机logo显示和开机动画播放功能移植到目标代码库 (`contest2026_034_ISoftStoneSmartFashionPI/`)。

## 移植文件清单

### 新增文件

| 文件 | 说明 |
|------|------|
| `app/watch/watch_main.c` | 主应用入口，LVGL初始化+事件循环 |
| `app/watch/apps/boot/boot_logo.c` | 开机logo显示（iSoftStone logo） |
| `app/watch/apps/boot/boot_logo.h` | 开机logo头文件 |
| `app/watch/apps/boot/boot_animation.c` | 开机动画播放（42帧序列帧，10fps） |
| `app/watch/apps/boot/boot_animation.h` | 开机动画头文件 |
| `app/watch/apps/launcher/launcher.c` | 状态机管理（INIT→LOGO→ANIM→DONE） |
| `app/watch/apps/launcher/launcher.h` | 状态机头文件 |
| `app/watch/apps/common/watch_pages.h` | 屏幕尺寸定义（410x502） |
| `app/watch/resource/resource.c` | 资源管理器（精简版，仅图片资源） |
| `app/watch/resource/resource.h` | 资源管理器头文件 |
| `app/watch/resource/image/generated/lvgl_assets.h` | 图片声明（仅isofstone_logo） |
| `app/watch/resource/image/img_src.inc` | 图片索引 |
| `app/watch/resource/image/app/boot/isoftstone_logo.c` | iSoftStone logo图像数据（C数组） |
| `app/watch/resource/power_png/generated/power_png_assets.h` | 动画帧声明（96帧） |
| `app/watch/resource/power_png/generated/power_png_assets.c` | 动画帧图像数据（3MB C数组，96帧PNG） |
| `app/watch/resource/power_png/power_img_src.inc` | 动画帧索引 |
| `app/watch/CMakeLists.txt` | CMake编译配置 |
| `app/watch/Kconfig` | Kconfig配置选项 |
| `app/watch/Make.defs` | Make编译引用 |
| `app/watch/PORTING_NOTES.md` | 本移植说明文档 |

### 修改文件

| 文件 | 修改内容 |
|------|----------|
| `board/esp32s3-touch-amoled/configs/openvela/defconfig` | 添加 `CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT=y`, `CONFIG_ETC_ROMFS=y`, `CONFIG_FS_ROMFS=y`, `CONFIG_LV_DEF_REFR_PERIOD=16` |
| `board/esp32s3-touch-amoled/src/etc/init.d/rcS` | 添加 `watch &` 启动命令 |

### 新增符号链接

| 链接 | 目标 |
|------|------|
| `apps/packages/demos/contest2026_034_watch` | `../../contest2026_034_ISoftStoneSmartFashionPI/app/watch` |

## 移植过程中的关键调整

### 1. Launcher状态机简化

原始代码中的 `launcher.c` 包含了完整的智能手表功能（表盘、闹钟、设置、传感器抬腕检测、充电监控等），移植版本只保留了开机logo→开机动画→完成三个状态，移除了所有与手表功能相关的依赖（dial、alarm、sensor、axp2101、settings、charging等）。

### 2. 资源管理模块精简

原始 `resource.c` 包含字体管理（FreeType初始化、TTF字体加载）和多模块图片资源管理。移植版本仅保留了图片资源管理功能：

- `watch_resource_get_img()` — 获取开机logo
- `watch_resource_get_img_power()` — 获取动画帧

移除了字体初始化、FreeType、power_jpg支持等不需要的模块。

### 3. 图片资源裁剪

原始 `lvgl_assets.h` 声明了150+张图片（表盘数字、天气图标、设置图标、运动图标等），移植版本仅保留开机logo (`isoftstone_logo`)。动画帧数据通过 `power_png_assets.h` + `power_png_assets.c` 单独管理。

### 4. 主应用简化

原始 `watch_main.c` 支持libuv事件循环和同步两种模式。移植版本移除了libuv路径，仅保留同步 `lv_timer_handler` + `usleep` 模式，以减少依赖。

### 5. 编译集成方式

按照目标代码库 `hello_app` 的集成模式：
- `app/watch/` 作为独立应用包
- 通过 `apps/packages/demos/contest2026_034_watch` 符号链接映射
- CMake使用 `nuttx_add_application()` 注册应用
- 通过 `target_include_directories()` 添加应用根目录到包含路径

### 6. 配置选项命名

使用 `CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT` 而非原始代码的 `CONFIG_LVX_USE_ESP32S3_WATCH`，以符合目标代码库的 `CONFIG_EXAMPLES_*` 命名惯例。

## 依赖分析

### 运行时依赖

| 依赖 | 说明 | 目标状态 |
|------|------|----------|
| LVGL | 图形库（已存在于target defconfig） | ✅ 已配置 |
| LVGL NuttX LCD | LVGL NuttX显示后端 | ✅ 已配置 |
| LVGL NuttX Touchscreen | 触摸屏输入（非必需，保留） | ✅ 已配置 |
| ROMFS | 启动脚本文件系统 | ✅ 新增 |
| PSRAM | 大容量内存（动画帧3MB数据需要） | ✅ 已配置 |

### 编译依赖

| 依赖 | 说明 |
|------|------|
| `GRAPHICS_LVGL` | LVGL图形支持 |
| `LV_USE_NUTTX` | LVGL NuttX后端 |
| `LV_USE_NUTTX_LCD` | LVGL LCD显示 |

## 开机流程

```
电源上电 → ESP32-S3 BootROM → NuttX内核初始化
  → nsh_main → nsh_initialize → boardctl(BOARDIOC_INIT)
    → board_app_initialize() → esp32s3_bringup()
      → 硬件初始化（LCD/FB/RTC等）
    → nsh_initscript() → /etc/init.d/rcS
      → watch &
        → lv_init() → lv_nuttx_init()
          → watch_resource_init()
            → launcher_init()
              → STATE_SHOW_LOGO (1秒)
                → STATE_SHOW_ANIM (~9秒, 42帧@10fps)
                  → STATE_DONE
                    → LVGL事件循环（持续）
```

## 性能考虑

- **动画帧率**: 10fps（100ms/帧），42帧总时长约4.2秒+状态转换时间
- **内存占用**: 动画帧数据约3MB（预编译C数组存储在Flash，运行时不额外分配）
- **显示刷新**: `LV_DEF_REFR_PERIOD=16` 设置60fps刷新率
- **PSRAM**: 需要PSRAM支持（ESP32-S3 8MB PSRAM已配置）

## 测试建议

1. **功能测试**: 编译烧录后，上电验证logo是否正确显示、动画是否流畅播放
2. **兼容性测试**: 验证在ESP32-S3 Touch AMOLED硬件上正常工作
3. **性能测试**: 使用串口日志观察各阶段时间戳，确认状态切换时间符合预期
