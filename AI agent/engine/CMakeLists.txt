cmake_minimum_required(VERSION 3.16)
project(local_engine_cli LANGUAGES CXX)

# 这个 engine 是“本地核心引擎”的最小形态：先做成一个 CLI 可执行文件 engine_cli。
# 好处：
# - Python 侧可以通过 subprocess 调用它（最稳、最容易调试/答辩）
# - 输入输出用 JSON 约定字段（跨语言、跨平台）
# - 后续再考虑 pybind11/ffi 也不迟

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 当前 demo 只编译一个可执行文件：engine_cli
add_executable(engine_cli
  src/main.cpp
)
