/**
 * @file gelu.cpp
 *
 * Copyright (C) 2024. Huawei Technologies Co., Ltd. All rights reserved.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * GELU Kernel 侧实现（Ascend C）
 *
 * 计算公式（erf 精确版，与 torch.nn.functional.gelu(approximate='none') 一致）：
 *     GELU(x) = 0.5 * x * (1 + erf(x / sqrt(2)))
 *
 * 由于硬件没有直接可用的 erf 指令，这里采用 Abramowitz & Stegun 7.1.26 公式逼近 erf：
 *     erf(u) ≈ 1 - (a1*t + a2*t^2 + a3*t^3 + a4*t^4 + a5*t^5) * exp(-u^2),
 *     t = 1 / (1 + p*u),  u = |x| / sqrt(2)
 * 该逼近的最大绝对误差约 1.5e-7，可满足 float32 绝对误差 < 1e-4 的精度要求。
 *
 * 为了同时处理正负输入（erf 为奇函数），利用恒等式：
 *     GELU(x) = 0.5 * x + 0.5 * |x| * erf(|x| / sqrt(2))
 * 从而不需要额外的符号判断。
 *
 * 为保证 float16 场景的计算精度，kernel 内部统一 Cast 到 float32 计算，最后再 Cast 回原类型。
 */
#include "kernel_operator.h"
using namespace AscendC;

constexpr int32_t BUFFER_NUM = 2;

// Abramowitz & Stegun 7.1.26 拟合系数
constexpr float GELU_INV_SQRT2 = 0.70710678118654752440f; // 1/sqrt(2)
constexpr float GELU_ERP_P = 0.3275911f;
constexpr float GELU_ERP_A1 = 0.254829592f;
constexpr float GELU_ERP_A2 = -0.284496736f;
constexpr float GELU_ERP_A3 = 1.421413741f;
constexpr float GELU_ERP_A4 = -1.453152027f;
constexpr float GELU_ERP_A5 = 1.061405429f;

/**
 * @brief 纯 float 张量上的 GELU 计算，结果写入 out。
 *        out 允许与 x 为同一块内存（原地计算）。
 * @param a,b,c,d 四块 float 临时缓冲（长度均不小于 n）
 */
__aicore__ inline void GeluComputeFloat(const LocalTensor<float> &out, const LocalTensor<float> &x,
                                        const LocalTensor<float> &a, const LocalTensor<float> &b,
                                        const LocalTensor<float> &c, const LocalTensor<float> &d, uint32_t n)
{
    // a = |x|
    Abs(a, x, n);
    // b = u = |x| / sqrt(2)
    Muls(b, a, GELU_INV_SQRT2, n);
    // c = 1 + p * u
    Muls(c, b, GELU_ERP_P, n);
    Adds(c, c, 1.0f, n);
    // d = 1  ->  c = t = 1 / (1 + p * u)
    Duplicate(d, 1.0f, n);
    Div(c, d, c, n);
    // d = poly(t) = ((((a5*t + a4)*t + a3)*t + a2)*t + a1)*t    （Horner 展开）
    Duplicate(d, GELU_ERP_A5, n);
    Mul(d, d, c, n);
    Adds(d, d, GELU_ERP_A4, n);
    Mul(d, d, c, n);
    Adds(d, d, GELU_ERP_A3, n);
    Mul(d, d, c, n);
    Adds(d, d, GELU_ERP_A2, n);
    Mul(d, d, c, n);
    Adds(d, d, GELU_ERP_A1, n);
    Muls(d, d, c, n); // d = poly
    // b = exp(-u^2)
    Mul(b, b, b, n);
    Muls(b, b, -1.0f, n);
    Exp(b, b, n);
    // b = 1 - poly*exp(-u^2) = erf(u)，其中 u = |x|/sqrt(2) >= 0
    Mul(b, b, d, n);
    Muls(b, b, -1.0f, n);
    Adds(b, b, 1.0f, n);
    // out = 0.5 * (x + |x| * erf(|x|/sqrt(2)))
    Mul(b, b, a, n);
    Add(out, x, b, n);
    Muls(out, out, 0.5f, n);
}

/**
 * @brief 类型适配：float16 输入需要先 Cast 到 float32 计算，再 Cast 回 float16。
 *        通过模板特化，float32 场景直接复用输入输出缓冲，避免多余搬运。
 */
template <typename T>
struct GeluCast {
    __aicore__ static inline void Compute(const LocalTensor<T> &dst, const LocalTensor<T> &src,
                                          const LocalTensor<float> &fbuf, const LocalTensor<float> &a,
                                          const LocalTensor<float> &b, const LocalTensor<float> &c,
                                          const LocalTensor<float> &d, uint32_t n)
    {
        Cast(fbuf, src, RoundMode::CAST_NONE, n);
        GeluComputeFloat(fbuf, fbuf, a, b, c, d, n);
        Cast(dst, fbuf, RoundMode::CAST_RINT, n);
    }
};

template <>
struct GeluCast<float> {
    __aicore__ static inline void Compute(const LocalTensor<float> &dst, const LocalTensor<float> &src,
                                          const LocalTensor<float> &fbuf, const LocalTensor<float> &a,
                                          const LocalTensor<float> &b, const LocalTensor<float> &c,
                                          const LocalTensor<float> &d, uint32_t n)
    {
        (void)fbuf; // float32 场景无需 Cast 缓冲
        GeluComputeFloat(dst, src, a, b, c, d, n);
    }
};

template <typename T>
class KernelGelu {
public:
    __aicore__ inline KernelGelu() {}

    __aicore__ inline void Init(GM_ADDR x, GM_ADDR y, uint32_t baseLength, uint32_t remainder, uint32_t tileLength)
    {
        uint32_t blockIdx = GetBlockIdx();
        // 当前核处理的数据量及其在全局数据中的起始偏移
        this->coreLength = baseLength + (blockIdx < remainder ? 1U : 0U);
        uint32_t offset = blockIdx * baseLength + (blockIdx < remainder ? blockIdx : remainder);

        xGm.SetGlobalBuffer((__gm__ T *)x + offset, this->coreLength);
        yGm.SetGlobalBuffer((__gm__ T *)y + offset, this->coreLength);

        this->tileLength = tileLength;
        pipe.InitBuffer(inQueueX, BUFFER_NUM, tileLength * sizeof(T));
        pipe.InitBuffer(outQueueY, BUFFER_NUM, tileLength * sizeof(T));
        pipe.InitBuffer(fBuf, tileLength * sizeof(float));
        pipe.InitBuffer(sA, tileLength * sizeof(float));
        pipe.InitBuffer(sB, tileLength * sizeof(float));
        pipe.InitBuffer(sC, tileLength * sizeof(float));
        pipe.InitBuffer(sD, tileLength * sizeof(float));
    }

    __aicore__ inline void Process()
    {
        uint32_t fullTile = this->coreLength / this->tileLength;
        uint32_t tailLength = this->coreLength - fullTile * this->tileLength;
        for (uint32_t i = 0; i < fullTile; ++i) {
            Step(i * this->tileLength, this->tileLength);
        }
        if (tailLength > 0) {
            Step(fullTile * this->tileLength, tailLength);
        }
    }

private:
    __aicore__ inline void Step(uint32_t offset, uint32_t length)
    {
        CopyIn(offset, length);
        Compute(length);
        CopyOut(offset, length);
    }

    __aicore__ inline void CopyIn(uint32_t offset, uint32_t length)
    {
        LocalTensor<T> xLocal = inQueueX.AllocTensor<T>();
        // DataCopyPad 支持非 32B 对齐的数据搬运，天然适配 N 非 32 整倍数的场景
        DataCopyExtParams copyParams = {1, static_cast<uint32_t>(length * sizeof(T)), 0, 0, 0};
        DataCopyPadExtParams<T> padParams = {false, 0, 0, 0};
        DataCopyPad(xLocal, xGm[offset], copyParams, padParams);
        inQueueX.EnQue(xLocal);
    }

    __aicore__ inline void Compute(uint32_t length)
    {
        LocalTensor<T> xLocal = inQueueX.DeQue<T>();
        LocalTensor<T> yLocal = outQueueY.AllocTensor<T>();

        LocalTensor<float> fbuf = fBuf.Get<float>();
        LocalTensor<float> a = sA.Get<float>();
        LocalTensor<float> b = sB.Get<float>();
        LocalTensor<float> c = sC.Get<float>();
        LocalTensor<float> d = sD.Get<float>();

        GeluCast<T>::Compute(yLocal, xLocal, fbuf, a, b, c, d, length);

        outQueueY.EnQue<T>(yLocal);
        inQueueX.FreeTensor(xLocal);
    }

    __aicore__ inline void CopyOut(uint32_t offset, uint32_t length)
    {
        LocalTensor<T> yLocal = outQueueY.DeQue<T>();
        DataCopyExtParams copyParams = {1, static_cast<uint32_t>(length * sizeof(T)), 0, 0, 0};
        DataCopyPad(yGm[offset], yLocal, copyParams);
        outQueueY.FreeTensor(yLocal);
    }

private:
    TPipe pipe;
    TQue<TPosition::VECIN, BUFFER_NUM> inQueueX;
    TQue<TPosition::VECOUT, BUFFER_NUM> outQueueY;
    TBuf<TPosition::VECCALC> fBuf;
    TBuf<TPosition::VECCALC> sA;
    TBuf<TPosition::VECCALC> sB;
    TBuf<TPosition::VECCALC> sC;
    TBuf<TPosition::VECCALC> sD;
    GlobalTensor<T> xGm;
    GlobalTensor<T> yGm;
    uint32_t coreLength;
    uint32_t tileLength;
};

extern "C" __global__ __aicore__ void gelu(GM_ADDR x, GM_ADDR y, GM_ADDR workspace, GM_ADDR tiling)
{
    GET_TILING_DATA(tiling_data, tiling);
    KernelGelu<DTYPE_X> op;
    op.Init(x, y, tiling_data.baseLength, tiling_data.remainder, tiling_data.tileLength);
    op.Process();
}
