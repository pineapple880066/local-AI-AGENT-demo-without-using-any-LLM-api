# local-AI-AGENT-demo-without-using-any-LLM-api
只是本地运行一个修复代码的引擎demo，用于测试CLI能否工作
文件
# example/demo_cpp

故意带“缺少 include 导致编译失败”的最小 C++ 项目，用于验证 agent 的端到端 pipeline。

运行：
`./build.sh`（第一次会失败：因为 main.cpp(example/demo_cpp下的) 故意没写全 #include）

用 agent 修复后再运行 `./build.sh`（会成功并输出）
