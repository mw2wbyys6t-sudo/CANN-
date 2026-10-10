/**
 * @file gelu.cpp
 *
 * Copyright (C) 2024. Huawei Technologies Co., Ltd. All rights reserved.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * GELU Host 侧实现：
 *   1. 算子原型定义（OpDef）与 Shape / DataType 推导
 *   2. Tiling 切分（决定使用多少 AI Core、每个核处理多少数据、单次搬运多少数据）
 *
 * 数学公式（与 torch.nn.functional.gelu approximate='none' 完全一致）：
 *   GELU(x) = x * Φ(x) = 0.5 * x * (1 + erf(x / sqrt(2)))
 */
#include "gelu_tiling.h"
#include "register/op_def_registry.h"
#include "tiling/platform/platform_ascendc.h"

namespace optiling {
// 每个 Queue 的深度（double buffer）
constexpr uint32_t BUFFER_NUM = 2;
// Kernel 侧为计算精度额外申请的 float 中间缓冲区个数（1 个 Cast 缓冲 + 4 个数学计算临时缓冲）
constexpr uint32_t FLOAT_BUFFER_NUM = 5;
// 预留 1/8 的 UB 空间，避免 UB 被完全占满导致其它预留空间不足
constexpr uint64_t UB_RESERVE_DENOMINATOR = 8;

/**
 * @brief Tiling 函数：计算切分参数并写入 tiling 数据
 *
 * 切分策略：
 *   - 按数据类型确定 32B 对齐元素数 alignNum（fp16=16，fp32=8）
 *   - 依据 UB 大小与单个元素占用的字节数，算出单次搬运的最大分块 tileLength
 *   - 将 totalLength 尽量均匀地分给 aivNum 个 AI Core，余数分给前 remainder 个核
 *   - 每个核内部再按 tileLength 循环处理，最后一块可能是非 32B 对齐的尾块
 */
static ge::graphStatus TilingFunc(gert::TilingContext *context)
{
    GeluTilingData tiling;

    // 1. 获取平台信息
    auto ascendcPlatform = platform_ascendc::PlatformAscendC(context->GetPlatformInfo());
    uint64_t ubSize = 0;
    ascendcPlatform.GetCoreMemSize(platform_ascendc::CoreMemType::UB, ubSize);

    uint32_t aivNum = ascendcPlatform.GetCoreNumAiv();
    if (aivNum == 0) {
        aivNum = 1;
    }

    // 2. 获取输入信息（元素个数 / 数据类型）
    const gert::Tensor *xTensor = context->GetInputTensor(0);
    uint32_t totalLength = static_cast<uint32_t>(xTensor->GetShapeSize());
    ge::DataType dtype = xTensor->GetDataType();
    uint32_t typeSize = (dtype == ge::DT_FLOAT16) ? 2U : 4U;
    uint32_t alignNum = 32U / typeSize;

    // 3. 计算单次搬运的分块长度
    //    每个元素在 UB 中的占用 = 输入输出 Queue + float 中间缓冲
    uint32_t bytesPerElem = BUFFER_NUM * typeSize * 2U + FLOAT_BUFFER_NUM * sizeof(float);
    uint64_t usableUb = ubSize - ubSize / UB_RESERVE_DENOMINATOR;
    uint32_t tileLength = static_cast<uint32_t>(usableUb / bytesPerElem);
    tileLength = tileLength / alignNum * alignNum;          // 向下对齐到 32B
    if (tileLength == 0) {
        tileLength = alignNum;
    }

    // 4. 依据工作量决定实际使用的核数，避免小 shape 时启动过多核
    uint32_t needCores = (totalLength + tileLength - 1U) / tileLength;
    if (needCores == 0) {
        needCores = 1;
    }
    if (needCores < aivNum) {
        aivNum = needCores;
    }

    // 5. 数据在核间均匀切分，余数分给前面的核
    uint32_t baseLength = totalLength / aivNum;
    uint32_t remainder = totalLength % aivNum;

    // 6. 写入 tiling
    tiling.set_totalLength(totalLength);
    tiling.set_baseLength(baseLength);
    tiling.set_remainder(remainder);
    tiling.set_tileLength(tileLength);
    tiling.set_alignNum(alignNum);

    context->SetBlockDim(aivNum);
    tiling.SaveToBuffer(context->GetRawTilingData()->GetData(), context->GetRawTilingData()->GetCapacity());
    context->GetRawTilingData()->SetDataSize(tiling.GetDataSize());

    size_t *currentWorkspace = context->GetWorkspaceSizes(1);
    currentWorkspace[0] = 0;
    return ge::GRAPH_SUCCESS;
}
} // namespace optiling

namespace ge {
// 输出 shape 与输入完全一致
static ge::graphStatus InferShape(gert::InferShapeContext *context)
{
    const gert::Shape *xShape = context->GetInputShape(0);
    gert::Shape *yShape = context->GetOutputShape(0);
    *yShape = *xShape;
    return GRAPH_SUCCESS;
}

// 输出数据类型与输入完全一致
static ge::graphStatus InferDataType(gert::InferDataTypeContext *context)
{
    const ge::DataType xDtype = context->GetInputDataType(0);
    context->SetOutputDataType(0, xDtype);
    return GRAPH_SUCCESS;
}
} // namespace ge

namespace ops {
class Gelu : public OpDef {
public:
    explicit Gelu(const char *name) : OpDef(name)
    {
        this->Input("x")
            .ParamType(REQUIRED)
            .DataType({ge::DT_FLOAT16, ge::DT_FLOAT})
            .Format({ge::FORMAT_ND, ge::FORMAT_ND})
            .UnknownShapeFormat({ge::FORMAT_ND, ge::FORMAT_ND});
        this->Output("y")
            .ParamType(REQUIRED)
            .DataType({ge::DT_FLOAT16, ge::DT_FLOAT})
            .Format({ge::FORMAT_ND, ge::FORMAT_ND})
            .UnknownShapeFormat({ge::FORMAT_ND, ge::FORMAT_ND});

        this->SetInferShape(ge::InferShape).SetInferDataType(ge::InferDataType);

        this->AICore()
            .SetTiling(optiling::TilingFunc);
        this->AICore().AddConfig("ascend310b")
                      .AddConfig("ascend910b");
    }
};

OP_ADD(Gelu);
} // namespace ops
