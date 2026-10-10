/**
* @file main.cpp
*
* Copyright (C) 2023. Huawei Technologies Co., Ltd. All rights reserved.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
*
* 单算子（aclnn）调用入口。
* 用法：
*     ./execute_op <float16|float32> <dim0> [dim1 dim2 ...]
* 例如：
*     ./execute_op float32 2 3 17
*     ./execute_op float16 1 1029
*/
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "acl/acl.h"
#include "op_runner.h"

#include "common.h"

bool g_isDevice = false;
int deviceId = 0;

namespace {
std::vector<int64_t> g_shape;
aclDataType g_dtype = ACL_FLOAT16;
std::string g_dtypeName = "float16";

bool ParseArgs(int argc, char **argv)
{
    if (argc < 3) {
        ERROR_LOG("Usage: %s <float16|float32> <dim0> [dim1 ...]", argv[0]);
        return false;
    }
    g_dtypeName = argv[1];
    if (g_dtypeName == "float16") {
        g_dtype = ACL_FLOAT16;
    } else if (g_dtypeName == "float32") {
        g_dtype = ACL_FLOAT;
    } else {
        ERROR_LOG("Unsupported dtype: %s (only float16 / float32)", argv[1]);
        return false;
    }
    for (int i = 2; i < argc; ++i) {
        int64_t dim = std::strtoll(argv[i], nullptr, 10);
        if (dim <= 0) {
            ERROR_LOG("Invalid dim: %s", argv[i]);
            return false;
        }
        g_shape.push_back(dim);
    }
    if (g_shape.empty()) {
        ERROR_LOG("At least one dim is required");
        return false;
    }
    return true;
}
} // namespace

void CreateOpDesc(OperatorDesc &opDesc)
{
    aclFormat format = ACL_FORMAT_ND;
    opDesc.AddInputTensorDesc(g_dtype, static_cast<int>(g_shape.size()), g_shape.data(), format);
    opDesc.AddOutputTensorDesc(g_dtype, static_cast<int>(g_shape.size()), g_shape.data(), format);
}

bool SetInputData(OpRunner &runner)
{
    size_t fileSize = 0;
    ReadFile("../input/input_x.bin", fileSize, runner.GetInputBuffer<void>(0), runner.GetInputSize(0));
    if (fileSize != runner.GetInputSize(0)) {
        ERROR_LOG("input file size(%zu) mismatch tensor size(%zu)", fileSize, runner.GetInputSize(0));
        return false;
    }
    INFO_LOG("Set input success");
    return true;
}

bool ProcessOutputData(OpRunner &runner)
{
    WriteFile("../output/output.bin", runner.GetOutputBuffer<void>(0), runner.GetOutputSize(0));

    INFO_LOG("Write output success");
    return true;
}

void DestoryResource()
{
    bool flag = false;
    if (aclrtResetDevice(deviceId) != ACL_SUCCESS) {
        ERROR_LOG("Reset device %d failed", deviceId);
        flag = true;
    }
    INFO_LOG("Reset Device success");
    if (aclFinalize() != ACL_SUCCESS) {
        ERROR_LOG("Finalize acl failed");
        flag = true;
    }
    if (flag) {
        ERROR_LOG("Destory resource failed");
    } else {
        INFO_LOG("Destory resource success");
    }
}

bool InitResource()
{
    std::string output = "../output";
    if (access(output.c_str(), 0) == -1) {
        int ret = mkdir(output.c_str(), 0700);
        if (ret == 0) {
            INFO_LOG("Make output directory successfully");
        }
        else {
            ERROR_LOG("Make output directory fail");
            return false;
        }
    }

    // acl.json is dump or profiling config file
    if (aclInit("../scripts/acl.json") != ACL_SUCCESS) {
        ERROR_LOG("acl init failed");
        return false;
    }

    if (aclrtSetDevice(deviceId) != ACL_SUCCESS) {
        ERROR_LOG("Set device failed. deviceId is %d", deviceId);
        (void)aclFinalize();
        return false;
    }
    INFO_LOG("Set device[%d] success", deviceId);

    // runMode is ACL_HOST which represents app is running in host
    // runMode is ACL_DEVICE which represents app is running in device
    aclrtRunMode runMode;
    if (aclrtGetRunMode(&runMode) != ACL_SUCCESS) {
        ERROR_LOG("Get run mode failed");
        DestoryResource();
        return false;
    }
    g_isDevice = (runMode == ACL_DEVICE);
    INFO_LOG("Get RunMode[%d] success", runMode);

    return true;
}

bool RunOp()
{
    // create op desc
    OperatorDesc opDesc;
    CreateOpDesc(opDesc);

    // create Runner
    OpRunner opRunner(&opDesc);
    if (!opRunner.Init()) {
        ERROR_LOG("Init OpRunner failed");
        return false;
    }

    // Load inputs
    if (!SetInputData(opRunner)) {
        ERROR_LOG("Set input data failed");
        return false;
    }

    // Run op
    if (!opRunner.RunOp()) {
        ERROR_LOG("Run op failed");
        return false;
    }

    // process output data
    if (!ProcessOutputData(opRunner)) {
        ERROR_LOG("Process output data failed");
        return false;
    }

    INFO_LOG("Run op success");
    return true;
}

int main(int argc, char **argv)
{
    if (!ParseArgs(argc, argv)) {
        return FAILED;
    }

    std::string shapeStr;
    for (size_t i = 0; i < g_shape.size(); ++i) {
        shapeStr += std::to_string(g_shape[i]);
        shapeStr += (i + 1 == g_shape.size()) ? "" : "x";
    }
    INFO_LOG("Run Gelu op: dtype=%s shape=[%s]", g_dtypeName.c_str(), shapeStr.c_str());

    if (!InitResource()) {
        ERROR_LOG("Init resource failed");
        return FAILED;
    }
    INFO_LOG("Init resource success");

    if (!RunOp()) {
        DestoryResource();
        return FAILED;
    }

    DestoryResource();

    return SUCCESS;
}
