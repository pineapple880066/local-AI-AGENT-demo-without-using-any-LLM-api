#!/usr/bin/env bash
set -euo pipefail

# 无论你从哪里运行 build.sh，都切换到脚本所在目录。
# 这样 g++ 能稳定找到 main.cpp，输出产物也会落在同一目录。
cd "$(dirname "$0")"

# 清理上一次生成的产物：
# - demo：可执行文件
# - demo.dSYM：macOS 可能生成的调试符号目录（如果不清理，搜索/索引时容易被当成文本读取）
rm -rf demo demo.dSYM

# 编译并运行：
# -std=c++17：确保能用 C++17（例如 std::this_thread / std::chrono）
# -Wall -Wextra：打开常用警告（工程习惯）
# -O0 -g：便于调试（毕设演示时很常用）
g++ -std=c++17 -Wall -Wextra -O0 -g main.cpp -o demo
./demo
