#!/bin/bash
# 对公共仓（nuttx / apps / lvgl）的团队修改以 patch 形式打入本地工作区。
# 使用方法：repo sync 之后、./build.sh 之前运行一次：
#   contest2026_034_ISoftStoneSmartFashionPI/tools/apply_patches.sh
# 幂等：已应用过的 patch 会自动跳过。

set -e

WORKSPACE="$(cd "$(dirname "$0")/.." && pwd)"
PATCHES="$WORKSPACE/contest2026_034_ISoftStoneSmartFashionPI/patches"

apply_one() {
  # $1 = 工作区内的项目路径, $2 = patches/ 下的子目录
  local proj="$WORKSPACE/$1"
  local dir="$PATCHES/$2"
  local p
  for p in "$dir"/*.patch; do
    [ -e "$p" ] || continue
    if git -C "$proj" apply --reverse --check "$p" 2>/dev/null; then
      echo "[skip] $1 <- $(basename "$p") (已应用)"
    elif git -C "$proj" apply "$p"; then
      echo "[ok]   $1 <- $(basename "$p")"
    else
      echo "[FAIL] $1 <- $(basename "$p")" >&2
      return 1
    fi
  done
}

apply_one nuttx nuttx
apply_one apps apps
apply_one apps/graphics/lvgl/lvgl lvgl

echo "全部 patch 处理完成。"
