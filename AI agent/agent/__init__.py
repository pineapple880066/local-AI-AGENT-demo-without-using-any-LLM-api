"""
agent 包：Python 侧的“编排层（orchestrator）”。

本仓库的目标是做一个最小可跑通的 coding-agent pipeline：
Plan → Retrieve → Patch → Run → Fix

当前 demo 的实现是“先跑通链路，再逐步增强”：
- Plan：固定模板计划（不是 LLM 生成），便于你先理解流程。
- Retrieve：通过 C++ 引擎（engine_cli）做简单的读文件/文本检索。
- Patch：用“按行替换 edits.json”的形式实现可控修改 + 快照备份（可回滚）。
- Run：运行工作区里的 ./build.sh 触发编译/运行。
- Fix：只实现了一个典型修复类别：缺少 #include 导致的编译失败。

后续你可以逐步替换成 LLM 驱动（DeepSeek）：
- Plan：让模型生成 ≤5 步计划
- Retrieve：根据计划自动生成 query 并检索
- Propose Patch：让模型输出统一格式 diff
- Fix：失败后根据错误日志做最多 2 次自动修复
但建议先保持“工具接口 + 日志落盘”不变，这样工程结构清晰、也更好答辩。
"""

