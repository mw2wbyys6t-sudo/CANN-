## 目录结构介绍
```
├── AclNNInvocation             //通过aclnn调用的方式调用Gelu算子
│   ├── inc                     // 头文件目录
│   │   ├── common.h            // 公共方法声明（含二进制文件读写）
│   │   ├── op_runner.h         // 算子运行相关信息声明
│   │   └── operator_desc.h     // 算子输入/输出描述声明
│   ├── input                   // 脚本生成的输入数据目录
│   ├── output                  // 算子运行输出与真值数据目录
│   ├── scripts
│   │   ├── acl.json            // acl 配置文件
│   │   ├── gen_data.py         // 输入数据与真值数据生成脚本（纯标准库，无 numpy/torch 依赖）
│   │   └── verify_result.py    // 精度比对脚本
│   ├── src
│   │   ├── CMakeLists.txt      // 编译规则文件
│   │   ├── common.cpp
│   │   ├── main.cpp            // 单算子调用入口（支持命令行指定 dtype 与 shape）
│   │   ├── op_runner.cpp       // 单算子调用主体流程
│   │   └── operator_desc.cpp
│   └── run.sh                  // 批量测试入口脚本
```
## 代码实现介绍
自定义算子编译部署后会自动生成单算子 API，采用"两段式接口"：

```cpp
// 第一段：获取执行器与 workspace 大小
aclnnStatus aclnnGeluGetWorkspaceSize(const aclTensor *x, const aclTensor *y,
                                      uint64_t *workspaceSize, aclOpExecutor **executor);
// 第二段：执行算子
aclnnStatus aclnnGelu(void *workspace, uint64_t workspaceSize,
                      aclOpExecutor *executor, aclrtStream stream);
```

## 运行样例算子
### 1. 编译并部署算子工程
请先参考工程根目录 [README.md](../../README.md) 完成算子工程的编译（`./build.sh`）与安装（`./custom_opp_*.run`）。

### 2. 运行端到端精度测试

```bash
cd test/AclNNInvocation
./run.sh              # 编译并运行全部测试用例
./run.sh --rebuild    # 强制重新编译后运行
```

`run.sh` 会自动完成「生成数据 → 运行算子 → 比对精度」的完整流程，并逐用例报告结果。
覆盖的测试矩阵：

| 覆盖项 | 说明 |
| --- | --- |
| 数据类型 | float16 / float32 |
| 维度 | 1 维 / 2 维 / 3 维 / 4 维（含批次维） |
| 非对齐 | N 非 32 整倍数（如 5、1029、10243、2×3×17） |
| 边界维度 | N=1、N=10240 |
| 数值范围 | [-1,1] 常规区间、[-5,5]、[-20,20] 边界数值 |

全部用例通过时会输出 `you have passed the Precision!`。

### 3. 单独运行某个用例（调试用）

```bash
# 生成数据：dtype lo hi dims...
python3 scripts/gen_data.py float32 -1.0 1.0 2 3 17
# 运行算子（cwd 需为 output/）
cd output && ./execute_op float32 2 3 17 && cd ..
# 精度比对
python3 scripts/verify_result.py output/output.bin output/golden.bin float32
```

## 更新说明
| 时间       | 更新事项     |
| ---------- | ------------ |
| 2024/10/23 | 新增本 readme |
| -          | 支持命令行指定 dtype/shape，覆盖完整测试矩阵 |
