# GELU 自定义算子（华为昇腾 CANN / Ascend C）

[![Build](https://github.com/mw2wbyys6t-sudo/CANN-/actions/workflows/build.yml/badge.svg)](https://github.com/mw2wbyys6t-sudo/CANN-/actions/workflows/build.yml)

**课程信息**

| 课程 | 姓名 | 学号 | 班级 | 指导教师 | 学期 | 院校 |
|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| 开源创新思维 | 朱胤陈 | F25015349 | F250118 | 祝欣蓉 | 2026-2027 秋季学期 | 上海杉达学院 |

本仓库为 GELU 算子赛题的完整算子工程实现，基于 **Ascend C** 编程语言开发，包含算子原型定义、Tiling 切分、Kernel 核函数、工程编译脚本与端到端精度验证。

---

## 作业索引

| 周次 | 作业内容 | 位置 |
|---|---|---|
| 第二周 | GELU 算子（华为昇腾 CANN / Ascend C 自定义算子） | 本仓库根目录（`op_host/`、`op_kernel/`、`build_910b.sh` 等） |
| 第三周 | 《开源创新思维》开源合规与许可证（ScanCode 扫描 pip 26.2.1） | [week3/](./week3/README.md) · [原始 docx](./week3/F25015349-朱胤陈-开源合规与许可证.docx) |

---

## 一、算子功能

对输入张量的每个元素应用 GELU（Gaussian Error Linear Unit）激活函数。

$$ \text{GELU}(x) = x \cdot \Phi(x) = 0.5 \cdot x \cdot \left(1 + \text{erf}\left(\frac{x}{\sqrt{2}}\right)\right) $$

- 输入/输出：`x` / `y`，形状相同，数据类型相同，数据格式 `ND`
- 支持数据类型：`float16`、`float32`
- 维度：任意多维 `(..., N)`，`N ∈ [1, 10240]`，兼容 `N` 非 32 整倍数的**非对齐**场景
- 精度要求：`float32` 相对/绝对误差 < 1e-4；`float16` 相对/绝对误差 < 1e-3

参考算子：`torch.nn.functional.gelu`（`approximate='none'`，即 erf 精确版）。

---

## 二、目录结构

```
.
├── CMakeLists.txt            # 算子工程顶层 CMake
├── CMakePresets.json         # 编译配置（芯片型号 ascend910b、CANN 路径等）
├── build.sh                  # 编译入口脚本（工程标准入口）
├── build_910b.sh             # 910B 一键编译脚本（自动定位 CANN + 核对配置 + 编译 + 报产物）
├── gelu.json                 # 算子原型定义（msopgen 输入）
├── cmake/                    # 工程编译框架脚本（msopgen 生成，无需修改）
├── framework/                # 算子适配插件（TF），单算子调用不依赖
├── scripts/                  # 算子包安装/升级脚本
├── op_host/                  # Host 侧实现
│   ├── gelu.cpp              #   算子原型定义 + Shape/DType 推导 + Tiling
│   └── gelu_tiling.h         #   TilingData 数据结构定义
├── op_kernel/                # Kernel 侧实现
│   └── gelu.cpp              #   Ascend C 核函数（核心计算）
└── test/
    ├── reference_gelu.py     # 纯 Python 参考实现 + 精度自检（无需 CANN）
    ├── compare_with_official.py  # 官方样例公式 vs 本实现的精度对照
    └── AclNNInvocation/      # 基于 aclnn 的端到端调用样例（需真实昇腾环境）
```

---

## 三、算法与关键设计

### 1. 为什么不能用 tanh 近似？

GELU 常见的 tanh 近似 `0.5x(1+tanh(0.79788(x+0.044715x³)))` 最大绝对误差约 **4.7e-4**，**无法满足**赛题 1e-4 的绝对误差要求。本实现采用 **erf 精确计算**。

### 2. erf 的数值逼近（Abramowitz & Stegun 7.1.26）

硬件没有直接的 erf 指令，采用经典有理逼近（最大绝对误差约 **1.5e-7**）：

```
erf(u) ≈ 1 - (a1·t + a2·t² + a3·t³ + a4·t⁴ + a5·t⁵)·exp(-u²),   t = 1/(1 + p·u)
u = |x| / √2
p  = 0.3275911
a1 =  0.254829592   a2 = -0.284496736   a3 = 1.421413741
a4 = -1.453152027   a5 =  1.061405429
```

由于 `erf` 是奇函数，利用恒等式避免符号判断：

```
GELU(x) = 0.5·x + 0.5·|x|·erf(|x|/√2)
```

### 3. float16 精度保障

Kernel 内部对 `float16` 输入统一 `Cast` 到 `float32` 完成全部数学运算，最后 `Cast` 回 `float16`（`CAST_RINT`）。`float32` 输入则直接原地复用输入输出缓冲，避免多余搬运。

### 4. 非对齐（N 非 32 整倍数）处理

- 搬运统一使用 `DataCopyPad`，按**字节数**精确搬运，天然支持任意长度；
- 数据在核间尽量均匀切分（前 `remainder` 个核各多处理 1 个元素），每个核内部再按 `tileLength` 分块，最后一块为任意长度的尾块。

---

## 四、编译与部署

> 需要已安装 CANN Toolkit（含 `Ascend C` 编译器）。以下步骤在装有昇腾环境的机器上执行。
> 本工程已按 **昇腾 910B**（`ascend910b`）配套：`CMakePresets.json` 的 `ASCEND_COMPUTE_UNIT = ascend910b`，算子原型 `AddConfig("ascend910b")`。

### 1. 编译算子工程（推荐一键脚本）

```bash
cd /workspace
./build_910b.sh
```

`build_910b.sh` 会自动完成：定位并加载 CANN 环境 → 核对/同步 `CMakePresets.json`（CANN 路径 + `ascend910b`）→ 调用 `build.sh` 编译 → 打印生成的算子包。

若你已手动配好环境与 `CMakePresets.json`，也可直接用工程标准入口：

```bash
./build.sh
```

编译成功后会在 `build_out/` 下生成算子安装包 `custom_opp_<os>_<arch>.run`。

### 2. 安装算子包

```bash
cd build_out
./custom_opp_<os>_<arch>.run
```

### 3. 端到端调用验证（可选，需真实昇腾环境）

```bash
cd test/AclNNInvocation
./run.sh
```

`run.sh` 会自动「生成数据 → 运行算子 → 比对精度」，并覆盖完整测试矩阵（float16/float32、1~4 维、非对齐、N=1/10240 边界、边界数值）。全部通过会输出 `you have passed the Precision!`。详见 [test/AclNNInvocation/README.md](file:///workspace/test/AclNNInvocation/README.md)。

---

## 五、本地精度自检（无需 CANN）

[reference_gelu.py](file:///workspace/test/reference_gelu.py) 用与 Kernel **完全相同的算子序列**在 CPU 上逐算子仿真（float32 舍入），可直接在任意装有 Python3 的机器上验证算法正确性与精度：

```bash
python3 test/reference_gelu.py
```

脚本覆盖：`float16/float32`、1~4 维 / 高维批次、非对齐 `N`、边界维度 `N=1 / 10240`、关键点 `x=0,±1,±2`、`[-1,1]` 与 `[-20,20]` 数值区间。预期全部 `PASS`。

关键点结果（与赛题示例一致）：

| x | 0 | 1 | -1 | 2 | -2 |
|---|---|---|---|---|---|
| GELU(x) | 0.0 | 0.8413 | -0.1587 | 1.9545 | -0.0455 |

### 与官方样例公式的对照

[compare_with_official.py](file:///workspace/test/compare_with_official.py) 扫描 `x ∈ [-20,20]`，对比不同方案的精度：

```bash
python3 test/compare_with_official.py
```

| 方案 | max 绝对误差 | 是否达标 (fp32 < 1e-4) |
|---|---|---|
| 官方 GeluSample（sigmoid 近似） | 7.46e-4 | ❌ FAIL |
| 常见 tanh 近似 | 4.73e-4 | ❌ FAIL |
| **本工程实现（erf，A&S 7.1.26）** | **2.11e-7** | ✅ **PASS** |

即：官方样例的近似公式无法满足赛题 1e-4 精度要求，本工程改为 erf 精确方案。

---

## 六、核心实现位置

- 算子原型 / Tiling：[op_host/gelu.cpp](file:///workspace/op_host/gelu.cpp)
- TilingData 定义：[op_host/gelu_tiling.h](file:///workspace/op_host/gelu_tiling.h)
- Ascend C 核函数：[op_kernel/gelu.cpp](file:///workspace/op_kernel/gelu.cpp)

---

## 七、第三周作业（开源合规与许可证）

本仓库同时归档课程《开源创新思维》第三周作业。

- **主题**：使用 **ScanCode Toolkit 32.5.0** 对 **pip 26.2.1** 源码包进行开源许可证合规扫描，覆盖其 `_vendor` 目录下 18 个内置（vendored）第三方组件，分析许可证分布、兼容性与合规义务，并给出风险与改进建议。
- **主要结论**：pip 主体为 MIT；内置组件以 MIT / Apache-2.0 / BSD-2/3-Clause 等宽松许可证为主，与 MIT 兼容，可整体再分发，核心义务为“保留声明、标注修改”；工具文件级覆盖率仅 15%，存在组合表达式与假阳性需人工复核，建议接入 CI 并输出 SPDX/CycloneDX。
- **文件**：
  - 报告说明（Markdown，可在线阅读）：[week3/README.md](./week3/README.md)
  - 原始提交文档：[week3/F25015349-朱胤陈-开源合规与许可证.docx](./week3/F25015349-朱胤陈-开源合规与许可证.docx)
  - 报告截图：[week3/media/report.png](./week3/media/report.png)
