#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
精度比对脚本（仅依赖 Python 标准库）。

用法：
    python3 verify_result.py <real_result.bin> <golden.bin> <float16|float32>

判定标准（allclose 风格，逐元素）：
    |real - golden| <= atol + rtol * |golden|
    float32  rtol = atol = 1e-4
    float16  rtol = atol = 1e-3
"""

import struct
import sys

TOL = {'float16': 1e-3, 'float32': 1e-4}


def read_bin(path, is_f16):
    with open(path, "rb") as f:
        data = f.read()
    item = 2 if is_f16 else 4
    n = len(data) // item
    fmt = '<%d%s' % (n, 'e' if is_f16 else 'f')
    return list(struct.unpack(fmt, data[:n * item]))


def verify_result(real_path, golden_path, dtype):
    if dtype not in TOL:
        print("[ERROR] unsupported dtype: %s" % dtype)
        return False
    is_f16 = (dtype == 'float16')
    rtol = TOL[dtype]
    atol = TOL[dtype]

    real = read_bin(real_path, is_f16)
    golden = read_bin(golden_path, is_f16)
    if len(real) != len(golden):
        print("[ERROR] size mismatch: real=%d golden=%d" % (len(real), len(golden)))
        return False

    n = len(real)
    n_bad = 0
    max_abs = 0.0
    max_rel = 0.0
    for r, g in zip(real, golden):
        diff = abs(r - g)
        deno = abs(g) + 1e-12
        max_abs = max(max_abs, diff)
        max_rel = max(max_rel, diff / deno)
        if diff > atol + rtol * abs(g):
            n_bad += 1

    print("[INFO]  dtype=%s count=%d max_abs=%.3e max_rel=%.3e (atol=rtol=%.1e)"
          % (dtype, n, max_abs, max_rel, atol))

    if n_bad != 0:
        print("[ERROR] result error: %d/%d elements out of tolerance" % (n_bad, n))
        return False
    print("test pass")
    return True


if __name__ == '__main__':
    if len(sys.argv) < 4:
        print("Usage: verify_result.py <real.bin> <golden.bin> <float16|float32>")
        sys.exit(1)
    ok = verify_result(sys.argv[1], sys.argv[2], sys.argv[3])
    sys.exit(0 if ok else 1)
