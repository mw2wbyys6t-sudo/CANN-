/**
 * @file gelu_tiling.h
 *
 * Copyright (C) 2024. Huawei Technologies Co., Ltd. All rights reserved.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */
#ifndef GELU_TILING_H
#define GELU_TILING_H
#include "register/tilingdata_base.h"

namespace optiling {
BEGIN_TILING_DATA_DEF(GeluTilingData)
    // 参与计算的总元素个数
    TILING_DATA_FIELD_DEF(uint32_t, totalLength);
    // 每个核处理的基础元素个数（前 remainder 个核多处理 1 个元素）
    TILING_DATA_FIELD_DEF(uint32_t, baseLength);
    // 余数：前 remainder 个核每个核多处理 1 个元素
    TILING_DATA_FIELD_DEF(uint32_t, remainder);
    // 单次搬运/计算的分块长度（元素个数，已按 32Bytes 对齐）
    TILING_DATA_FIELD_DEF(uint32_t, tileLength);
    // 一个 32Bytes 数据块包含的元素个数（float16=16，float32=8）
    TILING_DATA_FIELD_DEF(uint32_t, alignNum);
END_TILING_DATA_DEF;

REGISTER_TILING_DATA_CLASS(Gelu, GeluTilingData)
} // namespace optiling

#endif // GELU_TILING_H
