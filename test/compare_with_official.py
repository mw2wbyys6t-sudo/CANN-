#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
对照脚本：昇腾官方 GeluSample 的近似公式 vs 本工程实现的 erf 精确方案。

结论用于说明"为什么本工程没有直接沿用官方样例的公式"：
  - 官方样例使用 sigmoid/tanh 类近似，最大绝对误差约 7.5e-4 / 4.7e-4；
  - 本工程使用 Abramowitz & Stegun 7.1.26 逼近 erf，最大绝对误差约 2e-7；
  - 赛题要求 float32 绝对误差 < 1e-4，因此必须使用 erf 精确方案。

直接运行：python3 test/compare_with_official.py
"""

import math

INV_SQRT2 = 0.70710678118654752440
ERP_P = 0.3275911
_A = (0.254829592, -0.284496736, 1.421413741, -1.453152027, 1.061405429)


def gelu_exact(x):
    """精确参考（golden）"""
    return 0.5 * x * (1.0 + math.erf(x / math.sqrt(2.0)))


def gelu_official(x):
    """昇腾官方 GeluSample 公式（sigmoid 近似）"""
    p1, p2 = 0.0455399241, -1.595769122
    return x / (1.0 + math.exp(p2 * (x + p1 * x ** 3)))


def gelu_tanh(x):
    """常见 tanh 近似"""
    return 0.5 * x * (1.0 + math.tanh(math.sqrt(2.0 / math.pi) * (x + 0.044715 * x ** 3)))


def gelu_mine(x):
    """本工程 kernel 使用的 erf 方案（A&S 7.1.26 + 奇函数恒等式）"""
    a = abs(x) * INV_SQRT2
    t = 1.0 / (1.0 + ERP_P * a)
    poly = ((((_A[4] * t + _A[3]) * t + _A[2]) * t + _A[1]) * t + _A[0]) * t
    e = 1.0 - poly * math.exp(-a * a)
    return 0.5 * x + 0.5 * abs(x) * e


def scan(func, xs):
    max_abs, max_rel, at = 0.0, 0.0, 0.0
    for x in xs:
        g = gelu_exact(x)
        d = abs(func(x) - g)
        r = d / abs(g) if abs(g) > 1e-4 else 0.0
        if d > max_abs:
            max_abs, at = d, x
        max_rel = max(max_rel, r)
    return max_abs, max_rel, at


def main():
    xs = [-20.0 + i * 0.0005 for i in range(80001)]
    print("=" * 88)
    print("对照：不同 GELU 计算方案的精度（扫描 x ∈ [-20, 20]，步长 5e-4）")
    print("=" * 88)
    print("%-28s %-14s %-14s %-10s" % ("方案", "max绝对误差", "max相对误差", "达标(fp32<1e-4)"))
    print("-" * 88)

    rows = [
        ("官方 GeluSample (sigmoid)", gelu_official),
        ("常见 tanh 近似", gelu_tanh),
        ("本工程实现 (erf A&S)", gelu_mine),
    ]
    for name, func in rows:
        ma, mr, at = scan(func, xs)
        ok = "PASS" if ma < 1e-4 else "FAIL"
        print("%-28s %-14.3e %-14.3e %-10s" % (name, ma, mr, ok))
        if name.startswith("本工程"):
            print("  (最大绝对误差出现在 x=%+.4f)" % at)

    print("-" * 88)
    print("关键点对照 golden=0.8413/-0.1587/1.9545/-0.0455:")
    for x in (1.0, -1.0, 2.0, -2.0):
        print("  x=%+d  官方=%.4f  tanh=%.4f  本工程=%.4f  exact=%.4f"
              % (x, gelu_official(x), gelu_tanh(x), gelu_mine(x), gelu_exact(x)))
    print("=" * 88)


if __name__ == "__main__":
    main()
