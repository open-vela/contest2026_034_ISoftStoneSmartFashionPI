# 软通潮玩智能手表（iSoftStone Smart Fashion PI）

> 2026 首届 openvela AI 硬件开发者大赛参赛作品
> 队伍编号 `034`，仓库 `contest2026_034_ISoftStoneSmartFashionPI`

## 一、作品简介

基于 openvela（NuttX）在 **ESP32-S3 触摸 AMOLED 开发板**（410×502 CO5300 AMOLED 竖屏）上实现的 AI 潮玩智能手表。它将"潮玩电子宠物"与"实用智能手表"合为一体：

- **双 UI 模式，无重启切换**：「潮玩表情模式」下设备是一个有情绪的伙伴——20+ 种 GIF 表情（待机、聆听、思考、说话、微笑、睡眠、生病、得意……）随语音交互和系统事件实时变化；「手表模式」下则是完整的智能手表——表盘、天气、闹钟（含响铃）、秒表、运动、手电筒、设置。两种模式可在设置中或长按按键 10 秒即时切换，无需重启。
- **小通 AI 语音助手**：支持唤醒词唤醒、流式语音对话（集成小米 MiMo 语音与火山引擎端到端语音两条链路），可语音控制屏幕亮度、音量、智能家居设备（如"打开厨房灯/电视"），API Key 支持设备端配置并持久化。
- **情绪化的系统反馈**：低电量、充电、跌倒/扶正（QMI8658 六轴检测）等事件会注入提示词，由大模型生成不固定的人格化文案播报，并联动 sick/proud 等表情——不是冰冷的提示音，而是"有性格"的反应。
- **完整的系统能力**：WiFi 自动连接/重连、SNTP 时间同步、自动熄屏与抬腕亮屏、三级待机、MWDT 看门狗防卡死、SD 卡日志、音量/亮度本地调节并与语音会话同步。

## 二、选题方向

**AI 硬件产品创新**（融合手表应用创新）。

理由：本作品不是单一的手表应用，而是一台完整的 AI 硬件整机——从板级 BSP（显示/触摸/音频/PMU/传感器驱动适配）、系统服务（电源管理、看门狗、日志），到端侧 AI 语音交互栈与情绪化 UI，均由团队在 openvela 上构建，符合 AI 硬件赛道"端侧智能 + 完整产品形态"的定位。

## 三、目录结构

本仓通过 manifest `<linkfile>` 把作品代码软链进 openvela 编译树，**无需改动 packages/nuttx/vendor 等公共仓的仓内内容**：

```text
board/esp32s3-touch-amoled/   — 板级适配：CO5300 AMOLED、FT3168 触摸、AXP2101 电源、
                                QMI8658 六轴、ES7210 音频 ADC、按键、defconfig、开机脚本 rcS
                                → 软链至 nuttx/boards/xtensa/esp32s3/esp32s3-touch-amoled
app/watch/                    — 手表主应用（LVGL）：开机 Logo/动画、表情模式、手表模式 UI、
                                设置（WiFi/亮度/熄屏/系统/关于）、语音交互界面、双模式切换
                                → 软链至 packages/demos/contest2026_034_watch
app_watch/                    — 手表页面框架（openvela vendor/watch 的定制副本）：
                                表盘/天气/闹钟/秒表/运动/手电筒等页面与资源管理
                                → 软链至 vendor/watch
examples/                     — 8 个硬件 bring-up 与语音链路测试程序：
                                esp32s3_watch_{axp2101,button,pcf85063,qmi8658,wifi}、
                                mimo_voice_test、mimo_stream_voice_test、volc_e2e_voice
                                → 软链至 apps/examples/
vendor/ai_agent/              — 小通 AI 语音栈：唤醒、ASR/LLM/TTS、MiMo 与火山引擎 E2E 接入
                                → 软链至 packages/ai_agent
vendor/esp-hal-3rdparty/      — ESP32-S3 HAL（含团队修复：clk_ctrl_os / esp_mem / esp_mbedtls 等）
                                → 软链至 nuttx/arch/xtensa/src/esp32s3/esp-hal-3rdparty
bootloader/                   — ESP32-S3 bootloader 与 partition-table 二进制（烧录用）
patches/                      — 团队对公共仓的必要修改，以 patch 文件形式携带：
                                nuttx（新增 PCF85063/AXP2101/QMI8658/CO5300/ES7210/ES8311 驱动、
                                      I2S TDM、PSRAM 堆、SDMMC 修复）、
                                apps（nxplayer/nxrecorder 修复）、
                                lvgl（FreeType FT_ERR_PREFIX 兼容宏）
tools/apply_patches.sh        — 一键把 patches/ 打入本地工作区（幂等，可重复执行）
tools/                        — 素材生成脚本（Logo/GIF 转 C 数组、提示音生成等）
flash_esp32s3.sh              — 编译 / LittleFS 打包 / 烧录一键脚本（见"运行方式"）
logs/cgke/                    — AI Coding 日志（claude-code、qoder 会话导出）
app/hello_app/、quickapp/、board/contest_board/  — 组委会模板骨架（未使用，保留占位）
```

## 四、运行方式

### 1. 拉取完整工程

```bash
repo init -u https://github.com/open-vela/contest2026_034_ISoftStoneSmartFashionPI \
  -b dev-ai-contest-2026 -m contest2026_034_ISoftStoneSmartFashionPI.xml
repo sync -c -j8
```

同步后本仓位于工作区 `contest2026_034_ISoftStoneSmartFashionPI/`，openvela 全量源码（含预置交叉工具链 prebuilts）在外层。

### 2. 打入公共仓补丁（每次 sync 后执行一次）

对 nuttx / apps / lvgl 的修改以 patch 形式随仓携带，构建前应用：

```bash
contest2026_034_ISoftStoneSmartFashionPI/tools/apply_patches.sh
```

脚本幂等：已应用过的 patch 会自动跳过。

### 3. 编译

在**工作区根目录**执行：

```bash
./build.sh contest2026_034_ISoftStoneSmartFashionPI/board/esp32s3-touch-amoled/configs/openvela -j8
```

编译产物为 `nuttx/nuttx.bin`。

### 4. 烧录与运行（真机）

使用仓内一键脚本（自动处理工具链 PATH、bootloader 同步、LittleFS 素材镜像打包）：

```bash
# 编译 + 打包 LittleFS + 刷写固件和素材（默认串口 /dev/ttyACM0，可用 -P 指定）
contest2026_034_ISoftStoneSmartFashionPI/flash_esp32s3.sh -a -P /dev/ttyACM0

# 常用子命令：-n 只编译；-s 只刷已有固件；-f 只打包刷素材；-m menuconfig；-c distclean
contest2026_034_ISoftStoneSmartFashionPI/flash_esp32s3.sh -h
```

依赖：`esptool.py`（烧录）、`littlefs-python`（素材镜像打包，`pip3 install littlefs-python`）。

烧录完成上电后：开机 Logo → 开机动画 → 默认进入潮玩表情模式，WiFi 自动重连并同步网络时间；说唤醒词即可与小通 AI 对话，长按按键 10 秒或在设置中切换到手表模式。

> 语音功能首次使用需在设备上配置小通 AI 的 API Key（设置内持久化保存）。

## 五、AI Coding 使用说明

本作品全程以 AI 结对开发方式完成，完整对话日志见 `logs/cgke/`（2026-06 至 2026-09，含 claude-code 与 qoder 会话，按日期归档）。

AI 在各环节的协作方式：

- **需求拆解与方案设计**：双 UI 模式架构、表情状态机、低电量人格化交互链路、三级待机/熄屏策略等，均先与 AI 讨论方案、对比取舍后再落地。
- **编码**：LVGL 页面（表盘、设置、WiFi、闹钟等）、板级驱动适配（PCF85063/AXP2101/QMI8658/CO5300/ES7210）、语音链路接入等大量代码由 AI 生成初稿，人工 review 后迭代。
- **疑难调试**：WiFi 连接卡死、LVGL 定时器中直接操作 I2C 的竞争隐患、GIF 的 NETSCAPE 循环扩展语义导致动画播两遍、defconfig 与增量编译混用导致固件无法启动等问题，借助 AI 定位根因并修复。
- **工程化与文档**：开机 Logo 旋转适配脚本、GIF/字体/提示音资源生成脚本、刷机脚本防护逻辑、移植说明文档等由 AI 辅助完成。

AI 带来的实际收益：在约 3 个月内完成了从 BSP 适配到整机产品的 100+ 次迭代提交，大量样板代码（资源转换、页面脚手架）近乎零成本产出；调试阶段 AI 对日志的分析显著缩短了疑难 bug 的定位时间。
