#!/bin/bash
#
# flash_esp32s3.sh - ESP32-S3 统一编译/打包/烧录脚本
#
# 用法:
#   ./flash_esp32s3.sh              默认: 编译 NuttX 并刷写固件
#   ./flash_esp32s3.sh -a           编译 + 打包 LittleFS + 刷写全部
#   ./flash_esp32s3.sh -p           只打包 LittleFS 镜像
#   ./flash_esp32s3.sh -f           打包并刷写 LittleFS 镜像
#   ./flash_esp32s3.sh -n           只编译 NuttX，不刷机
#   ./flash_esp32s3.sh -s           跳过编译，直接刷写已有固件
#   ./flash_esp32s3.sh -c           执行 distclean（不编译）
#   ./flash_esp32s3.sh -m           执行 menuconfig
#   ./flash_esp32s3.sh -P /dev/ttyUSB0 -b 460800   指定串口和波特率
#

set -e


SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 脚本固定在 <root>/contest2026_034_ISoftStoneSmartFashionPI/ 下运行
TEAM_DIR="${SCRIPT_DIR}"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
NUTTX_DIR="${PROJECT_ROOT}/nuttx"

if [ ! -d "${NUTTX_DIR}" ]; then
    echo "[ERROR] 找不到 NuttX 目录: ${NUTTX_DIR}"
    echo "       脚本必须放在 <project-root>/contest2026_034_ISoftStoneSmartFashionPI/ 下运行"
    exit 1
fi

# 交叉工具链 PATH（关键！否则 CPP 处理 rcS 会静默失败为空文件）
# openvela 官方 prebuilt xtensa-esp32s3-elf 工具链
TOOLCHAIN_DIR="${PROJECT_ROOT}/prebuilts/gcc/linux-x86_64/xtensa-esp32s3-elf/bin"
if [ -d "${TOOLCHAIN_DIR}" ]; then
    case ":${PATH}:" in
        *":${TOOLCHAIN_DIR}:"*) : ;;   # 已在 PATH 中
        *) export PATH="${TOOLCHAIN_DIR}:${PATH}" ;;
    esac
else
    echo "[WARN] 未找到交叉工具链: ${TOOLCHAIN_DIR}"
    echo "       如果编译失败请检查 prebuilts 是否已同步"
fi

# 构建工具 PATH（kconfig-tweak、genromfs 等）
BUILD_TOOLS_DIR="${PROJECT_ROOT}/prebuilts/build-tools/linux-x86_64/bin"
if [ -d "${BUILD_TOOLS_DIR}" ]; then
    case ":${PATH}:" in
        *":${BUILD_TOOLS_DIR}:"*) : ;;
        *) export PATH="${BUILD_TOOLS_DIR}:${PATH}" ;;
    esac
fi

# Kconfig Python 前端 PATH（menuconfig/kconfiglib，支持 osource 语法）
PYTHON_TOOLS_DIR="${PROJECT_ROOT}/prebuilts/tools/python/bin"
if [ -d "${PYTHON_TOOLS_DIR}" ]; then
    case ":${PATH}:" in
        *":${PYTHON_TOOLS_DIR}:"*) : ;;
        *) export PATH="${PYTHON_TOOLS_DIR}:${PATH}" ;;
    esac
fi
export PYTHONPATH="${PROJECT_ROOT}/prebuilts/tools/python/dist-packages/kconfiglib:${PROJECT_ROOT}/prebuilts/tools/python/dist-packages${PYTHONPATH:+:$PYTHONPATH}"

# 确认关键工具可用
if ! command -v xtensa-esp32s3-elf-gcc >/dev/null 2>&1; then
    echo "[ERROR] xtensa-esp32s3-elf-gcc 不在 PATH 中"
    echo "       期望位置: ${TOOLCHAIN_DIR}"
    exit 1
fi

# 默认参数
PORT="/dev/ttyACM0"
BAUD="921600"
ACTION="build_flash"   # build_flash | pack | flash_littlefs | flash_all | build_only | flash_only | convert_assets | distclean | menuconfig
FORCE_PACK=0
SKIP_PROMPT=0

# 当前配置地址（传递给 build.sh）
BOARD_CONFIG="contest2026_034_ISoftStoneSmartFashionPI/board/esp32s3-touch-amoled/configs/openvela"

# LittleFS 参数
BLOCK_SIZE=4096
READ_SIZE=1024
PROG_SIZE=1024

# 素材目录
RES_DIR="${TEAM_DIR}/app/watch/resource"
TMP_DIR="${PROJECT_ROOT}/.tmp_littlefs_pack"
IMG_FILE="${PROJECT_ROOT}/littlefs_watch.img"
PACK_PY="${PROJECT_ROOT}/nuttx/tools/pack_littlefs.py"

############################################################################
# Helper functions
############################################################################

usage() {
    cat << EOF
Usage: $(basename "$0") [OPTIONS]

ESP32-S3 统一编译/打包/烧录脚本

Options:
  -a, --flash-all       编译 NuttX + 打包 LittleFS + 刷写全部 (固件+素材)
  -p, --pack-only       只打包 LittleFS 镜像，不烧录
  -f, --flash-littlefs  打包并刷写 LittleFS 镜像
  -C, --convert-assets  转换 PNG/TTF 素材为 C 数组（手动触发）
  -s, --skip-build      跳过编译，直接刷写已有 nuttx.bin 固件
  -n, --build-only      只编译 NuttX，不刷机
  -c, --distclean       执行 ./build.sh <当前配置地址> distclean（不编译）
  -m, --menuconfig      执行 ./build.sh <当前配置地址> menuconfig
  -F, --force           强制重新生成 LittleFS 镜像
  -P, --port <port>     串口设备 (默认: ${PORT})
  -b, --baud <baud>     烧录波特率 (默认: ${BAUD})
  -y, --yes             跳过确认提示
  -h, --help            显示本帮助

Examples:
  $(basename "$0")                    # 编译并刷写固件
  $(basename "$0") -s                 # 跳过编译，直接刷写已有固件
  $(basename "$0") -a                 # 全量编译+打包+烧录
  $(basename "$0") -p                 # 只打包 LittleFS 镜像
  $(basename "$0") -C                 # 手动转换素材为 C 数组
  $(basename "$0") -aC                # 转换素材 + 全量编译烧录
  $(basename "$0") -f -P /dev/ttyUSB0 # 打包并烧录素材到指定串口
  $(basename "$0") -c                 # 仅执行 distclean
  $(basename "$0") -m                 # 仅执行 menuconfig

EOF
    exit 0
}

log_info()  { echo -e "\033[36m[INFO]\033[0m  $*"; }
log_warn()  { echo -e "\033[33m[WARN]\033[0m  $*"; }
log_error() { echo -e "\033[31m[ERROR]\033[0m $*"; }
log_ok()    { echo -e "\033[32m[OK]\033[0m   $*"; }

confirm() {
    if [ "$SKIP_PROMPT" -eq 1 ]; then
        return 0
    fi
    # 非交互式终端自动确认
    if [ ! -t 0 ]; then
        return 0
    fi
    read -rp "$1 [Y/n] " ans
    case "$ans" in
        [Nn]*) return 1 ;;
        *) return 0 ;;
    esac
}

read_mtd_config() {
    # 优先读取 defconfig（用户维护的源配置），因为 .config 可能还没同步
    local defconfig="${TEAM_DIR}/board/esp32s3-touch-amoled/configs/openvela/defconfig"
    local dotconfig="${NUTTX_DIR}/.config"
    local config_file=""

    if [ -f "$defconfig" ] && grep -q "CONFIG_ESP32S3_STORAGE_MTD_OFFSET=" "$defconfig" 2>/dev/null; then
        config_file="$defconfig"
    elif [ -f "$dotconfig" ] && grep -q "CONFIG_ESP32S3_STORAGE_MTD_OFFSET=" "$dotconfig" 2>/dev/null; then
        config_file="$dotconfig"
    fi

    if [ -n "$config_file" ]; then
        MTD_OFFSET=$(grep "CONFIG_ESP32S3_STORAGE_MTD_OFFSET=" "$config_file" | head -1 | cut -d'=' -f2 | tr -d '"')
        MTD_SIZE=$(grep "CONFIG_ESP32S3_STORAGE_MTD_SIZE=" "$config_file" | head -1 | cut -d'=' -f2 | tr -d '"')
        log_info "读取 MTD 配置: $config_file"
    else
        MTD_OFFSET="0x1210000"
        MTD_SIZE="0xDF0000"
        log_warn "未找到 MTD 配置，使用默认值"
    fi

    FS_SIZE=$(printf "%d" "${MTD_SIZE}")
    BLOCK_COUNT=$((FS_SIZE / BLOCK_SIZE))
}

check_esptool() {
    if command -v esptool.py &> /dev/null; then
        ESPTOOL="esptool.py"
    elif command -v esptool &> /dev/null; then
        ESPTOOL="esptool"
    else
        log_error "未找到 esptool.py / esptool"
        exit 1
    fi
}

check_littlefs_python() {
    if ! python3 -c "import littlefs" 2>/dev/null; then
        log_error "需要安装 littlefs-python"
        log_info "  pip3 install littlefs-python"
        exit 1
    fi
}

# 拷贝 bootloader / partition-table 到 nuttx/，供 make flash 使用
ensure_bootloader_bins() {
    local src="${TEAM_DIR}/bootloader"
    if [ ! -f "${src}/bootloader-esp32s3.bin" ] || [ ! -f "${src}/partition-table-esp32s3.bin" ]; then
        log_warn "未找到 bootloader/partition-table 源文件: ${src}"
        return 0
    fi
    cp "${src}/bootloader-esp32s3.bin"      "${NUTTX_DIR}/"
    cp "${src}/partition-table-esp32s3.bin" "${NUTTX_DIR}/"
    log_info "已同步 bootloader / partition-table 到 nuttx/"
}

# 把本仓库 app/*, board/* 映射到 openvela 工作树（代替 manifest linkfile）
# 幂等：已存在且指向正确则跳过。
ensure_app_symlinks() {
    # <目标链接>:<指向的源>（源为相对于目标链接所在目录的相对路径）
    local links=(
        "${PROJECT_ROOT}/packages/demos/contest2026_034_watch:../../contest2026_034_ISoftStoneSmartFashionPI/app/watch"
        "${PROJECT_ROOT}/packages/demos/contest2026_034_hello_app:../../contest2026_034_ISoftStoneSmartFashionPI/app/hello_app"
    )
    local entry link target parent
    for entry in "${links[@]}"; do
        link="${entry%%:*}"
        target="${entry##*:}"
        parent="$(dirname "${link}")"

        # 目录不存在时跳过（例如 packages/demos 尚未同步）
        if [ ! -d "${parent}" ]; then
            log_warn "目录不存在，无法创建链接: ${parent}"
            continue
        fi

        # 已存在且指向正确，跳过
        if [ -L "${link}" ] && [ "$(readlink "${link}")" = "${target}" ]; then
            continue
        fi

        # 实体目录/其他链接，不覆盖，仅提醒
        if [ -e "${link}" ] && [ ! -L "${link}" ]; then
            log_warn "已存在同名实体，跳过链接创建: ${link}"
            continue
        fi

        ln -sfn "${target}" "${link}"
        log_info "已创建应用链接: $(basename "${link}") -> ${target}"
    done
}

############################################################################
# Actions
############################################################################

action_first_time_config() {
    # 首次配置直接指向大赛仓库内的 defconfig 目录（out-of-tree 绝对路径）
    # 不依赖 manifest linkfile；也不用 build.sh，避免其 savedefconfig 阶段回写覆盖
    # 大赛仓库维护的 defconfig。
    local team_config_dir="${TEAM_DIR}/board/esp32s3-touch-amoled/configs/openvela"
    if [ ! -f "${team_config_dir}/defconfig" ]; then
        log_error "找不到大赛 defconfig: ${team_config_dir}/defconfig"
        exit 1
    fi
    if [ ! -x "${NUTTX_DIR}/tools/configure.sh" ]; then
        log_error "找不到 nuttx/tools/configure.sh"
        exit 1
    fi

    log_info "首次配置: ${team_config_dir}"
    "${NUTTX_DIR}/tools/configure.sh" -e "${team_config_dir}"
}

action_distclean() {
    log_info "执行 distclean: ${BOARD_CONFIG}"
    cd "${PROJECT_ROOT}"
    ./build.sh "${BOARD_CONFIG}" distclean
    log_ok "distclean 完成"
}

action_menuconfig() {
    log_info "执行 menuconfig: ${BOARD_CONFIG}"
    cd "${PROJECT_ROOT}"
    ./build.sh "${BOARD_CONFIG}" menuconfig
    log_ok "menuconfig 完成"
}

action_build_nuttx() {
    log_info "编译 NuttX..."

    # 先确保应用链接存在（否则 packages/demos/Kconfig 里拿不到 watch 的 Kconfig，
    # 导致 CONFIG_EXAMPLES_CONTEST2026_WATCH_BOOT 只在 defconfig 里写了但进不了 .config）
    ensure_app_symlinks

    # 防踩坑：defconfig 比 .config 新，说明配置改过但未生效。
    # 此时增量编译会混用不同配置编出的目标文件（可能链接失败或固件无法启动），
    # 必须 distclean 后重新配置并全量编译。
    local defconfig="${TEAM_DIR}/board/esp32s3-touch-amoled/configs/openvela/defconfig"
    if [ -f "${NUTTX_DIR}/.config" ] && [ -f "${defconfig}" ] \
        && [ "${defconfig}" -nt "${NUTTX_DIR}/.config" ]; then
        log_warn "检测到 defconfig 比 .config 新，配置可能已修改"
        log_warn "为避免增量编译混用不同配置的目标文件，先执行 distclean 全量重编..."
        action_distclean
    fi

    # 首次配置（.config 不存在时）
    if [ ! -f "${NUTTX_DIR}/.config" ]; then
        log_warn "NuttX 配置不存在，执行首次配置..."
        action_first_time_config
    fi

    cd "${NUTTX_DIR}"

    # 配置就绪后再拷贝 bootloader / partition-table，避免被 distclean 误清
    ensure_bootloader_bins

    log_info "同步配置 (make oldconfig)..."
    yes "" | make oldconfig 2>/dev/null || make oldconfig

    make -j$(nproc)
    log_ok "NuttX 编译完成"
}

action_flash_nuttx() {
    log_info "刷写 NuttX 固件..."
    cd "${NUTTX_DIR}"
    # -s 场景下没走 build，也要保证 bootloader / partition-table 就位
    ensure_bootloader_bins
    sudo chmod 777 "${PORT}" 2>/dev/null || true
    make flash ESPTOOL_PORT="${PORT}" ESPTOOL_BINDIR=./ -j$(nproc)
    log_ok "NuttX 固件刷写完成"
}

action_convert_assets() {
    log_info "=========================="
    log_info "转换素材为 C 数组"
    log_info "=========================="

    local assets_gen_dir="${RES_DIR}/image/generated"
    local assets_names="${RES_DIR}/image/img_src.inc"
    local font_gen_dir="${RES_DIR}/font/generated"
    local convert_script="${PROJECT_ROOT}/nuttx/tools/convert_lvgl_assets.py"
    local convert_font_script="${PROJECT_ROOT}/nuttx/tools/convert_fonts_to_c.py"

    # PNG -> C arrays
    log_info "[1/3] 转换 PNG 图片为 LVGL C 数组..."
    if [ -f "${convert_script}" ]; then
        mkdir -p "${assets_gen_dir}"
        python3 "${convert_script}" \
            -i "${RES_DIR}/image" \
            -o "${assets_gen_dir}" \
            --names-file "${assets_names}" \
            --cf RGB565 --align 4
        log_ok "图片 C 数组生成完成: ${assets_gen_dir}"
    else
        log_warn "转换脚本不存在: ${convert_script}，跳过 PNG 转换"
    fi

    # TTF -> C arrays
    log_info "[2/3] 转换 TTF 字体为 C 数组..."
    if [ -f "${convert_font_script}" ]; then
        mkdir -p "${font_gen_dir}"
        local charset_file="${RES_DIR}/font/charset.txt"
        if [ -f "${charset_file}" ]; then
            python3 "${convert_font_script}" \
                -i "${RES_DIR}/font/assets" \
                -o "${font_gen_dir}" \
                --names-file "${RES_DIR}/font/font_src.inc" \
                --subset-file "${charset_file}"
            log_ok "字体 C 数组生成完成 (subset: $(wc -m < "${charset_file}") chars): ${font_gen_dir}"
        else
            python3 "${convert_font_script}" \
                -i "${RES_DIR}/font/assets" \
                -o "${font_gen_dir}" \
                --names-file "${RES_DIR}/font/font_src.inc"
            log_ok "字体 C 数组生成完成 (full): ${font_gen_dir}"
        fi
    else
        log_warn "字体转换脚本不存在: ${convert_font_script}，跳过 TTF 转换"
    fi

    # Power animation -> C arrays
    log_info "[3/3] 转换开机动画为 C 数组..."
    python3 "${PROJECT_ROOT}/nuttx/tools/convert_power_assets.py" -r "${RES_DIR}"
    log_ok "开机动画 C 数组生成完成"

    log_ok "素材转换全部完成！"
}

action_pack_littlefs() {
    log_info "打包 LittleFS 镜像..."

    read_mtd_config
    check_littlefs_python

    log_info "配置: MTD_OFFSET=${MTD_OFFSET}, MTD_SIZE=${MTD_SIZE} (${FS_SIZE} bytes), BLOCK_COUNT=${BLOCK_COUNT}"

    # 清理/创建临时目录
    if [ -d "${TMP_DIR}" ] && [ "$FORCE_PACK" -eq 1 ]; then
        rm -rf "${TMP_DIR}"
    fi
    if [ -f "${IMG_FILE}" ] && [ "$FORCE_PACK" -eq 1 ]; then
        rm -f "${IMG_FILE}"
    fi

    mkdir -p "${TMP_DIR}"
    mkdir -p "${TMP_DIR}/FONT"
    mkdir -p "${TMP_DIR}/power_jpg"
    mkdir -p "${TMP_DIR}/power_png"

    # 复制字体（从 assets，不复制已嵌入的 C 数组字体，除非用户需要）
    log_info "[1/2] 复制字体文件..."
    if [ -d "${RES_DIR}/font/assets" ]; then
        cp "${RES_DIR}/font/assets/"*.ttf "${TMP_DIR}/FONT/" 2>/dev/null || true
        log_ok "已复制 $(ls "${TMP_DIR}/FONT/" 2>/dev/null | wc -l) 个字体文件"
    else
        log_warn "找不到字体目录 ${RES_DIR}/font/assets"
    fi

    # 开机动画已嵌入固件（C数组），无需复制到 LittleFS
    # log_info "[2/3] 复制开机动画..."
    # if [ -d "${RES_DIR}/power_jpg" ]; then
    #     cp "${RES_DIR}/power_jpg/"*.jpg "${TMP_DIR}/power_jpg/" 2>/dev/null || true
    #     log_ok "已复制 $(ls "${TMP_DIR}/power_jpg/" 2>/dev/null | wc -l) 个 jpg 文件"
    # fi
    # if [ -d "${RES_DIR}/power_png" ]; then
    #     cp "${RES_DIR}/power_png/"*.png "${TMP_DIR}/power_png/" 2>/dev/null || true
    #     log_ok "已复制 $(ls "${TMP_DIR}/power_png/" 2>/dev/null | wc -l) 个 png 文件"
    # fi

    # 生成镜像
    log_info "[2/2] 生成 LittleFS 镜像..."
    python3 "${PACK_PY}" \
        -c "${TMP_DIR}" \
        -o "${IMG_FILE}" \
        -b "${BLOCK_SIZE}" \
        -s "${BLOCK_COUNT}" \
        -r "${READ_SIZE}" \
        -p "${PROG_SIZE}"

    log_ok "镜像生成成功: ${IMG_FILE} ($(ls -lh "${IMG_FILE}" | awk '{print $5}'))"
}

action_flash_littlefs() {
    log_info "刷写 LittleFS 镜像..."
    read_mtd_config
    check_esptool

    if [ ! -f "${IMG_FILE}" ]; then
        log_error "找不到镜像文件: ${IMG_FILE}"
        log_info "请先执行: $(basename "$0") -p"
        exit 1
    fi

    sudo chmod 777 "${PORT}" 2>/dev/null || true

    "${ESPTOOL}" --chip esp32s3 --port "${PORT}" --baud "${BAUD}" \
        write_flash --compress "${MTD_OFFSET}" "${IMG_FILE}"

    log_ok "LittleFS 镜像刷写完成 (偏移: ${MTD_OFFSET})"
}

action_flash_all() {
    log_info "=========================="
    log_info "全量编译 + 打包 + 烧录"
    log_info "=========================="
    action_build_nuttx
    action_pack_littlefs

    log_info "编译和打包完成，准备刷写..."
    if ! confirm "即将刷写 NuttX 固件和 LittleFS 镜像，确认继续?"; then
        log_info "已取消，镜像保留在: ${IMG_FILE}"
        exit 0
    fi

    action_flash_nuttx
    action_flash_littlefs
    log_ok "全部完成！"
}

############################################################################
# Parse arguments
############################################################################

while [[ $# -gt 0 ]]; do
    case $1 in
        -a|--flash-all)
            ACTION="flash_all"
            shift
            ;;
        -p|--pack-only)
            ACTION="pack"
            shift
            ;;
        -f|--flash-littlefs)
            ACTION="flash_littlefs"
            shift
            ;;
        -C|--convert-assets)
            ACTION="convert_assets"
            shift
            ;;
        -s|--skip-build)
            ACTION="flash_only"
            shift
            ;;
        -n|--build-only)
            ACTION="build_only"
            shift
            ;;
        -c|--distclean)
            ACTION="distclean"
            shift
            ;;
        -m|--menuconfig)
            ACTION="menuconfig"
            shift
            ;;
        -F|--force)
            FORCE_PACK=1
            shift
            ;;
        -P|--port)
            PORT="$2"
            shift 2
            ;;
        -b|--baud)
            BAUD="$2"
            shift 2
            ;;
        -y|--yes)
            SKIP_PROMPT=1
            shift
            ;;
        -h|--help)
            usage
            ;;
        *)
            log_error "未知参数: $1"
            usage
            ;;
    esac
done

############################################################################
# Main
############################################################################

case "$ACTION" in
    flash_only)
        action_flash_nuttx
        ;;
    build_flash)
        action_build_nuttx
        action_flash_nuttx
        ;;
    build_only)
        action_build_nuttx
        ;;
    pack)
        action_pack_littlefs
        ;;
    flash_littlefs)
        action_pack_littlefs
        action_flash_littlefs
        ;;
    convert_assets)
        action_convert_assets
        ;;
    distclean)
        action_distclean
        ;;
    menuconfig)
        action_menuconfig
        ;;
    flash_all)
        action_flash_all
        ;;
    *)
        log_error "未知动作: $ACTION"
        exit 1
        ;;
esac
