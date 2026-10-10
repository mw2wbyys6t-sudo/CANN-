#!/bin/bash
# =============================================================================
#  GELU 算子 —— 昇腾 910B 一键编译脚本
#
#  功能：
#    1. 自动定位并加载 CANN 环境
#    2. 核对/同步 CMakePresets.json（CANN 路径 + ASCEND_COMPUTE_UNIT=ascend910b）
#    3. 调用工程标准编译入口 build.sh 生成算子包
#    4. 输出生成的 custom_opp_*.run 安装包路径
#
#  用法：
#    ./build_910b.sh
# =============================================================================
set -o pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$SCRIPT_DIR"

SOC="ascend910b"

echo "==================== GELU 算子编译 (910B) ===================="

# ---------------------------------------------------------------------------
# 1. 定位 CANN 安装路径
# ---------------------------------------------------------------------------
CANN_PATH="${ASCEND_HOME_PATH:-${ASCEND_HOME_DIR:-}}"
if [ -z "$CANN_PATH" ] || [ ! -d "$CANN_PATH" ]; then
    for p in "/usr/local/Ascend/ascend-toolkit/latest" \
             "$HOME/Ascend/ascend-toolkit/latest"; do
        if [ -d "$p" ]; then
            CANN_PATH="$p"
            break
        fi
    done
fi
if [ -z "$CANN_PATH" ] || [ ! -d "$CANN_PATH" ]; then
    echo "[ERROR] 未找到 CANN 安装路径，请先安装 CANN Toolkit 或 export ASCEND_HOME_PATH=<cann_path>"
    exit 1
fi
echo "[INFO] CANN 路径: $CANN_PATH"

# 加载 CANN 环境变量（含编译器）
if [ -f "$CANN_PATH/bin/setenv.bash" ]; then
    # shellcheck disable=SC1091
    source "$CANN_PATH/bin/setenv.bash"
fi
export ASCEND_HOME_PATH="$CANN_PATH"

# 校验必需命令
if ! command -v cmake > /dev/null 2>&1; then
    echo "[ERROR] 未找到 cmake，请先安装 cmake (>=3.16)"
    exit 1
fi
if ! command -v python3 > /dev/null 2>&1; then
    echo "[ERROR] 未找到 python3"
    exit 1
fi

# ---------------------------------------------------------------------------
# 2. 核对/同步 CMakePresets.json
# ---------------------------------------------------------------------------
python3 - "$CANN_PATH" "$SOC" <<'PY'
import json
import os
import sys

cann_path, soc = sys.argv[1], sys.argv[2]
preset_file = "CMakePresets.json"
with open(preset_file, "r") as f:
    cfg = json.load(f)

changed = []
for preset in cfg.get("configurePresets", []):
    cv = preset.get("cacheVariables", {})
    if "ASCEND_CANN_PACKAGE_PATH" in cv:
        old = cv["ASCEND_CANN_PACKAGE_PATH"].get("value", "")
        if old != cann_path:
            cv["ASCEND_CANN_PACKAGE_PATH"]["value"] = cann_path
            changed.append("ASCEND_CANN_PACKAGE_PATH: %s -> %s" % (old, cann_path))
    if "ASCEND_COMPUTE_UNIT" in cv:
        old = cv["ASCEND_COMPUTE_UNIT"].get("value", "")
        if old != soc:
            cv["ASCEND_COMPUTE_UNIT"]["value"] = soc
            changed.append("ASCEND_COMPUTE_UNIT: %s -> %s" % (old, soc))

with open(preset_file, "w") as f:
    json.dump(cfg, f, indent=4, ensure_ascii=False)
    f.write("\n")

if changed:
    print("[INFO] 已更新 CMakePresets.json:")
    for c in changed:
        print("       - " + c)
else:
    print("[INFO] CMakePresets.json 配置已符合 910B，无需修改")
PY
if [ $? -ne 0 ]; then
    echo "[ERROR] 同步 CMakePresets.json 失败"
    exit 1
fi

# ---------------------------------------------------------------------------
# 3. 编译算子工程
# ---------------------------------------------------------------------------
chmod +x ./build.sh
echo "[INFO] 开始编译，请稍候 ..."
./build.sh
rc=$?
if [ $rc -ne 0 ]; then
    echo "[ERROR] 算子工程编译失败 (exit=$rc)"
    exit $rc
fi

# ---------------------------------------------------------------------------
# 4. 报告产物
# ---------------------------------------------------------------------------
pkg=$(ls -1t build_out/custom_opp_*.run 2>/dev/null | head -1)
echo "=============================================================="
if [ -n "$pkg" ]; then
    echo "[SUCCESS] 编译成功！算子安装包："
    echo "          $pkg"
    echo "[INFO] 安装命令："
    echo "          cd build_out && ./$(basename "$pkg")"
else
    echo "[WARN] 未在 build_out 下找到 custom_opp_*.run，请检查编译日志"
    exit 1
fi
