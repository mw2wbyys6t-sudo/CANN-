#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
测试数据生成脚本（仅依赖 Python 标准库，无需 numpy / torch）。

用法：
    python3 gen_data.py <float16|float32> <lo> <hi> <dim0> [dim1 dim2 ...]

例如：
    python3 gen_data.py float32 -1.0 1.0 2 3 17
    python3 gen_data.py float16 -5.0 5.0 1029

生成：
    ./input/input_x.bin   输入张量（按 dtype 存储）
    ./output/golden.bin   真值（精确 GELU，用 math.erf 在 float64 下计算后再转换到 dtype）
"""

import math
import os
import random
import struct
import sys


def gelu_exact(x):
    """精确参考：0.5x(1+erf(x/sqrt2))，float64 计算"""
    return 0.5 * x * (1.0 + math.erf(x / math.sqrt(2.0)))


def cast(v, is_f16):
    if is_f16:
        try:
            return struct.unpack('e', struct.pack('e', v))[0]
        except OverflowError:
            return math.inf if v > 0 else -math.inf
    return struct.unpack('f', struct.pack('f', v))[0]


def pack(values, is_f16):
    fmt = '<%d%s' % (len(values), 'e' if is_f16 else 'f')
    return struct.pack(fmt, *values)


def main():
    if len(sys.argv) < 5:
        print("Usage: gen_data.py <float16|float32> <lo> <hi> <dim0> [dim1 ...]")
        return 1

    dtype = sys.argv[1]
    if dtype == 'float16':
        is_f16 = True
    elif dtype == 'float32':
        is_f16 = False
    else:
        print("Unsupported dtype: %s (only float16 / float32)" % dtype)
        return 1

    lo = float(sys.argv[2])
    hi = float(sys.argv[3])
    dims = [int(v) for v in sys.argv[4:]]
    if not dims or any(d <= 0 for d in dims):
        print("Invalid dims: %s" % dims)
        return 1

    total = 1
    for d in dims:
        total *= d

    random.seed(20240924 + total + (1 if is_f16 else 0))
    raw = [random.uniform(lo, hi) for _ in range(total)]
    # 输入本身按其数据类型取整，模拟真实送入 device 的数据
    inputs = [cast(v, is_f16) for v in raw]
    # golden 在 dtype 表示范围内计算，再转回 dtype
    golden = [cast(gelu_exact(v), is_f16) for v in inputs]

    os.makedirs("./input", exist_ok=True)
    os.makedirs("./output", exist_ok=True)
    with open("./input/input_x.bin", "wb") as f:
        f.write(pack(inputs, is_f16))
    with open("./output/golden.bin", "wb") as f:
        f.write(pack(golden, is_f16))

    print("[INFO]  gen_data: dtype=%s shape=%s count=%d range=[%g,%g]"
          % (dtype, 'x'.join(str(d) for d in dims), total, lo, hi))
    return 0


if __name__ == "__main__":
    sys.exit(main())
