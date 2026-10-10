#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
GELU 算子参考实现与精度自检脚本（仅依赖 Python 标准库，无需 CANN / numpy / torch）。

作用：
  1. 用与 kernel 完全相同的算子序列（Abramowitz & Stegun 7.1.26 + erf 恒等式）
     仿真 GELU 的计算过程（按 float32 逐算子舍入），预测 NPU 上的计算结果；
  2. 与"精确参考"（math.erf 的 GELU，作为 golden）比对；
  3. 覆盖赛题要求的测试用例范围：float16/float32、1~4 维、非 32 对齐、
     边界维度 N=1 / N=10240、边界数值、关键点、[-1,1] 常规区间。

判定标准（与官方 verify_result.py 一致，allclose 风格）：
  绝对误差 <= tol  或  相对误差 <= tol     即视为通过
  其中 float32 的 tol = 1e-4，float16 的 tol = 1e-3。
"""

import math
import struct
import random

RTOL_F32, ATOL_F32 = 1e-4, 1e-4
RTOL_F16, ATOL_F16 = 1e-3, 1e-3

# Abramowitz & Stegun 7.1.26 系数（与 kernel 中保持一致）
INV_SQRT2 = 0.70710678118654752440
ERP_P = 0.3275911
ERP_A1 = 0.254829592
ERP_A2 = -0.284496736
ERP_A3 = 1.421413741
ERP_A4 = -1.453152027
ERP_A5 = 1.061405429


def r32(v):
    """舍入到 float32（模拟单精度运算）"""
    return struct.unpack('f', struct.pack('f', v))[0]


def to_f16(v):
    try:
        return struct.unpack('e', struct.pack('e', v))[0]
    except OverflowError:
        return math.inf if v > 0 else -math.inf


def to_dtype(v, is_f16):
    return to_f16(v) if is_f16 else r32(v)


def gelu_exact(x):
    """精确参考：0.5x(1+erf(x/sqrt2))，用 float64 计算"""
    return 0.5 * x * (1.0 + math.erf(x / math.sqrt(2.0)))


def gelu_kernel(x, is_f16):
    """与 op_kernel/gelu.cpp 等价的逐算子仿真（全程 float32 运算）"""
    # Cast 到 float32
    x = r32(x)
    # a = |x|
    a = r32(abs(x))
    # b = u = |x|/sqrt2
    b = r32(a * r32(INV_SQRT2))
    # c = 1 + p*u
    c = r32(b * r32(ERP_P))
    c = r32(c + r32(1.0))
    # c = t = 1/(1+p*u)
    c = r32(r32(1.0) / c)
    # d = poly(t)   (Horner)
    d = r32(ERP_A5)
    d = r32(r32(d * c) + r32(ERP_A4))
    d = r32(r32(d * c) + r32(ERP_A3))
    d = r32(r32(d * c) + r32(ERP_A2))
    d = r32(r32(d * c) + r32(ERP_A1))
    d = r32(d * c)
    # b = exp(-u^2)
    b = r32(b * b)
    b = r32(b * r32(-1.0))
    b = r32(math.exp(b))
    # b = erf(u) = 1 - poly*exp(-u^2)
    b = r32(b * d)
    b = r32(b * r32(-1.0))
    b = r32(b + r32(1.0))
    # out = 0.5*(x + |x|*erf(u))
    b = r32(b * a)
    out = r32(x + b)
    out = r32(out * r32(0.5))
    # Cast 回目标类型
    return to_dtype(out, is_f16)


def check(name, values, is_f16):
    rtol = RTOL_F16 if is_f16 else RTOL_F32
    atol = ATOL_F16 if is_f16 else ATOL_F32
    n = len(values)
    max_abs, max_rel = 0.0, 0.0
    atol_ok = True
    rtol_ok = True
    for v in values:
        x = to_dtype(v, is_f16)          # 输入本身也应是目标类型可表示的值
        gold = to_dtype(gelu_exact(x), is_f16)
        real = gelu_kernel(x, is_f16)
        diff = abs(real - gold)
        deno = max(abs(real), abs(gold)) + 1e-12
        max_abs = max(max_abs, diff)
        max_rel = max(max_rel, diff / deno)
        if diff > atol:
            atol_ok = False
        if diff / deno > rtol:
            rtol_ok = False
    passed = atol_ok or rtol_ok
    tag = "PASS" if passed else "FAIL"
    print(f"[{tag}] {name:<34s} n={n:<6d} max_abs={max_abs:.3e} max_rel={max_rel:.3e}")
    return passed


def rand_values(shape, is_f16, lo, hi):
    total = 1
    for s in shape:
        total *= s
    return [random.uniform(lo, hi) for _ in range(total)]


def main():
    random.seed(20240924)
    all_pass = True

    print("=" * 78)
    print("关键点验证（应与赛题示例一致）")
    demo = [0.0, 1.0, -1.0, 2.0, -2.0]
    print("  x    :", demo)
    print("  golden:", [round(gelu_exact(x), 4) for x in demo])
    print("  kernel:", [round(gelu_kernel(x, False), 4) for x in demo])
    all_pass &= check("关键点 x=0,±1,±2", demo, False)

    print("=" * 78)
    print("数据类型覆盖 / 维度覆盖 / 非对齐 / 边界维度（[-1,1] 常规区间）")
    dims = {
        "1D": [1024 + 5],                 # 非 32 对齐
        "2D": [3, 17],                    # 非 32 对齐
        "3D": [2, 5, 33],                 # 非 32 对齐
        "4D": [2, 3, 4, 7],               # 非 32 对齐
        "高维带批次": [8, 4, 16, 13],
        "N=1": [1],
        "N=32对齐": [32],
        "N=10240": [10240],
        "非对齐N=10243": [10243],
    }
    for is_f16 in (False, True):
        dt = "float16" if is_f16 else "float32"
        for dname, shape in dims.items():
            vals = rand_values(shape, is_f16, -1.0, 1.0)
            all_pass &= check(f"{dt} {dname} {shape}", vals, is_f16)

    print("=" * 78)
    print("边界数值 / 较大数值")
    for is_f16 in (False, True):
        dt = "float16" if is_f16 else "float32"
        big = rand_values([2048 + 7], is_f16, -20.0, 20.0)
        all_pass &= check(f"{dt} 大范围[-20,20]", big, is_f16)
        mid = rand_values([2048 + 3], is_f16, -5.0, 5.0)
        all_pass &= check(f"{dt} [-5,5]", mid, is_f16)

    print("=" * 78)
    print("整体结果：", "全部通过 ✅" if all_pass else "存在失败 ❌")
    return 0 if all_pass else 1


if __name__ == "__main__":
    raise SystemExit(main())
