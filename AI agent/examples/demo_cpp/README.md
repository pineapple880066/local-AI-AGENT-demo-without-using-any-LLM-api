# demo_cpp

故意带“缺少 include 导致编译失败”的最小 C++ 项目，用于验证 agent 的端到端 pipeline。

运行：
- `./build.sh`（第一次会失败：因为 main.cpp 故意没写全 #include）
- 用 agent 修复后再运行 `./build.sh`（会成功并输出）

你会看到的闭环（答辩可讲）：
1) Run：build.sh 编译失败，得到错误日志
2) Fix：agent 根据错误推断需要补哪些 #include
3) Patch：engine_cli 应用修改，并生成 snapshot（可回滚）
4) Run：再次编译成功，程序跑起来
