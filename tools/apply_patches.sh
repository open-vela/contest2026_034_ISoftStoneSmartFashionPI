#!/bin/bash
# 对公共仓（nuttx / apps / lvgl）的团队修改以 patch 形式打入本地工作区。
# 使用方法：repo sync 之后、./build.sh 之前运行一次：
#   contest2026_034_ISoftStoneSmartFashionPI/tools/apply_patches.sh
# 幂等：已应用过的 patch 会自动跳过。
# 注意：本脚本位于 <openvela工作区>/contest2026_034_ISoftStoneSmartFashionPI/tools/ 下。

set -e

# 工作区根目录 = 本脚本上三级（tools/ -> 队伍仓 -> 工作区根）
TEAM_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WORKSPACE="$(cd "$TEAM_DIR/.." && pwd)"
PATCHES="$TEAM_DIR/patches"

if [ ! -d "$WORKSPACE/nuttx" ] || [ ! -d "$WORKSPACE/apps" ]; then
  echo "[FAIL] 未找到 openvela 工作区（期望 $WORKSPACE 下存在 nuttx/ 和 apps/）" >&2
  exit 1
fi

apply_one() {
  # $1 = 工作区内的项目路径, $2 = patches/ 下的子目录
  local proj="$WORKSPACE/$1"
  local dir="$PATCHES/$2"
  local p
  local found=0
  if [ ! -d "$proj" ]; then
    echo "[FAIL] 项目目录不存在: $proj" >&2
    return 1
  fi
  for p in "$dir"/*.patch; do
    [ -e "$p" ] || continue
    found=1
    if git -C "$proj" apply --reverse --check "$p" 2>/dev/null; then
      echo "[skip] $1 <- $(basename "$p") (已应用)"
    elif git -C "$proj" apply "$p"; then
      echo "[ok]   $1 <- $(basename "$p")"
    else
      echo "[FAIL] $1 <- $(basename "$p")" >&2
      return 1
    fi
  done
  if [ "$found" -eq 0 ]; then
    echo "[FAIL] 未找到 patch 文件: $dir/*.patch" >&2
    return 1
  fi
}

apply_one nuttx nuttx
apply_one apps apps
apply_one apps/graphics/lvgl/lvgl lvgl

# 写入成功标记，供构建期守卫（board scripts/Make.defs）检查
MARKER="$WORKSPACE/.contest_patches_applied"
date -Iseconds > "$MARKER"

echo "全部 patch 处理完成。"
echo "标记文件: $MARKER"
