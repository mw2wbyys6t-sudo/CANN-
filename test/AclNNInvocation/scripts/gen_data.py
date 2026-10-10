import torch
import torch.nn as nn
import numpy as np
import os


def gen_golden_data_simple():
    input_x = np.random.uniform(-1, 1, [32]).astype(np.float16)
    gelu = nn.GELU()
    res = gelu(torch.Tensor(input_x))
    golden = res.numpy().astype(np.float16)
    print(golden)
    os.system("mkdir -p input")
    os.system("mkdir -p output")
    input_x.tofile("./input/input_x.bin")
    golden.tofile("./output/golden.bin")


if __name__ == "__main__":
    gen_golden_data_simple()
