#!/bin/bash
# =============================================================================
#  GELU 自定义算子 —— aclnn 端到端精度测试
#
#  流程：生成测试数据 -> 运行算子 -> 比对精度
#  覆盖：float16/float32、1~4 维、非 32 对齐、边界维度 N=1/10240、边界数值
#
#  使用：
#      ./run.sh              # 编译并跑全部用例
#      ./run.sh --rebuild    # 强制重新编译后跑全部用例
# =============================================================================
export ASCEND_SLOG_PRINT_TO_STDOUT=0
export ASCEND_GLOBAL_LOG_LEVEL=0

CURRENT_DIR=$(
    cd $(dirname ${BASH_SOURCE:-$0})
    pwd
)
cd $CURRENT_DIR

REBUILD=0
if [ "$1" == "--rebuild" ]; then
    REBUILD=1
fi

# 测试用例：dtype  lo   hi   dims...
CASES="
float32  -1.0 1.0   5
float16  -1.0 1.0   2 2
float32  -1.0 1.0   1 2 2
float32  -1.0 1.0   1
float32  -1.0 1.0   10240
float16  -1.0 1.0   10243
float32  -1.0 1.0   2 3 17
float16  -1.0 1.0   8 4 16 13
float32  -5.0 5.0   4096 3
float16  -5.0 5.0   1029
float32  -20.0 20.0 2048 5
float16  -1.0 1.0   300
"

# 1. 准备 CANN 环境变量
if [ ! $ASCEND_HOME_DIR ]; then
    if [ -d "$HOME/Ascend/ascend-toolkit/latest" ]; then
        export ASCEND_HOME_DIR=$HOME/Ascend/ascend-toolkit/latest
    else
        export ASCEND_HOME_DIR=/usr/local/Ascend/ascend-toolkit/latest
    fi
fi
source $ASCEND_HOME_DIR/bin/setenv.bash

export DDK_PATH=$ASCEND_HOME_DIR
arch=$(uname -m)
export NPU_HOST_LIB=$ASCEND_HOME_DIR/${arch}-linux/lib64

# 2. 编译 acl 可执行文件（只编一次）
build_execute_op() {
    cd $CURRENT_DIR
    rm -rf build; mkdir -p build; cd build
    cmake ../src > /dev/null
    if [ $? -ne 0 ]; then
        echo "ERROR: cmake failed!"
        return 1
    fi
    make
    if [ $? -ne 0 ]; then
        echo "ERROR: make failed!"
        return 1
    fi
    echo "INFO: build execute_op success!"
    return 0
}

if [ $REBUILD -eq 1 ] || [ ! -e "./output/execute_op" ]; then
    build_execute_op || exit 1
else
    echo "INFO: reuse ./output/execute_op (use --rebuild to force rebuild)"
fi

# 3. 逐用例执行
total=0
pass=0
failed_cases=""

run_case() {
    local dtype=$1 lo=$2 hi=$3
    shift 3
    local dims=("$@")
    total=$((total + 1))

    echo ""
    echo "==================================================================="
    echo "CASE ${total}: dtype=${dtype} range=[${lo},${hi}] shape=(${dims[*]})"
    echo "==================================================================="

    # 3.1 生成测试数据
    local gen_args=("$dtype" "$lo" "$hi")
    for d in "${dims[@]}"; do gen_args+=("$d"); done
    if ! python3 scripts/gen_data.py "${gen_args[@]}"; then
        echo "ERROR: gen_data failed"
        failed_cases="${failed_cases} [${total}] gen_data"
        return 1
    fi

    # 3.2 运行算子
    cd "$CURRENT_DIR/output"
    timeout 60 ./execute_op "$dtype" "${dims[@]}"
    local rc=$?
    cd "$CURRENT_DIR"
    if [ $rc -ne 0 ]; then
        echo "ERROR: execute_op failed (rc=${rc})"
        failed_cases="${failed_cases} [${total}] execute"
        return 1
    fi

    # 3.3 比对精度
    local ret
    ret=$(python3 scripts/verify_result.py output/output.bin output/golden.bin "$dtype")
    echo "$ret"
    if echo "$ret" | grep -q "test pass"; then
        pass=$((pass + 1))
        echo ">>> CASE ${total} PASS"
        return 0
    else
        failed_cases="${failed_cases} [${total}] verify"
        echo ">>> CASE ${total} FAIL"
        return 1
    fi
}

while read -r dtype lo hi dims; do
    [ -z "$dtype" ] && continue
    run_case "$dtype" "$lo" "$hi" $dims
done <<< "$CASES"

# 4. 汇总
echo ""
echo "==================================================================="
echo "SUMMARY: ${pass}/${total} cases passed"
if [ $pass -eq $total ]; then
    echo "INFO: you have passed the Precision!"
    echo "==================================================================="
    exit 0
else
    echo "ERROR: failed cases ->${failed_cases}"
    echo "==================================================================="
    exit 1
fi
