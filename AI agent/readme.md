1.目录下example中的main.cpp是要修复的文件；

## 以下是毕业设计框架

本科毕业设计开题（Coding Agent / Cursor 精髓版）

目标：做成 Cursor 的“精髓”——不是 UI 花哨，而是可复现、可控的工程 pipeline。MVP 先做 CLI 版，重点打通工作流：Plan → Retrieve → Patch → Run → Fix。

---

快速跑通一个最小 demo（建议先跑这个验证你能做出来）

1) 编译引擎（C++）：
- `cmake -S engine -B engine/build && cmake --build engine/build -j`

2) 运行示例项目（第一次会失败）：
- `cd examples/demo_cpp && ./build.sh`

3) 让 agent 自动修复并再次验证：
- `python3 -m agent.cli '修复示例项目编译错误' --workspace examples/demo_cpp`
- `cd examples/demo_cpp && ./build.sh`

日志输出在：`.agent_logs/<run_id>/`

---

第一周任务清单（立刻开干）

1) 定 repo 结构 + 工具 JSON 协议
2) engine：实现 list_files / read_file / search_text
3) agent：固定 workflow（plan → retrieve → patch）
4) 跑通一个 demo：对一个小项目“修一个 bug”并成功编译

---

# 0. 目标边界先定死（第 1 天）

MVP 只盯三件事（答辩可量化）：
1) 让 agent “看懂项目”（索引/检索）
2) 让修改“可控可回滚”（patch/rollback）
3) 让它“像工具”可复现（run/test + 日志 + 自修复循环）

非 MVP（后期加分项）：
- 花哨 UI、IDE 插件、过度自由的 agent（容易失控、难答辩）

---

## 1. 总体架构（先画出来再写）

建议三层（先 CLI，后续可扩展 TUI/VSCode 插件）：

### A. 前端层（CLI → TUI/VSCode 插件）
- 输入：用户需求（自然语言）+ 当前工作区
- 输出：计划（plan）、变更列表（changes）、diff、执行结果（run logs）
- 交互目标：让用户“看见并确认”每一步（尤其是 diff 与运行结果）

### B. Agent 编排层（Python）
- LLM 调用（DeepSeek）
- 工具选择与固定工作流：
  1) Plan：生成不超过 5 步的计划
  2) Retrieve：根据计划生成检索 query，调用引擎拿上下文
  3) Propose Patch：模型只输出统一格式 diff（严格）
  4) Apply：引擎应用 patch
  5) Run/Fix：运行编译/测试并在失败时最多重试 2 次修复
- 关键原则：上下文拼接受控、输出结构化、日志落盘可复现

### C. C++ 核心引擎（本地能力/工程味）
- 代码索引/检索（符号/引用/文件结构）
- diff/patch 应用与回滚（可控修改）
- 编译/测试任务管理（跨平台抽象）
- Python 调度 C++：优先“CLI 子进程 + JSON 通信”（最稳、最易答辩）
  - 不要一上来 pybind11/ffi（毕设阶段容易卡在工程细节）

---

## 2. 里程碑式开发流程（建议 4~6 周节奏）

## Milestone 1：项目骨架 + 工具协议（2~3 天）
- 交付物：
  - monorepo 目录结构
  - Python agent 能调用 C++ engine（返回 JSON）
  - 统一 tool schema（输入/输出字段固定）
- 建议目录：
  - /agent（Python）
  - /engine（C++）
  - /examples（可运行 demo 项目）
  - /docs（开题/中期/答辩材料）

## Milestone 2：C++ 引擎 v1（索引 + 检索）（1 周）
目标：让 agent “看懂项目”
1) 扫描目录生成文件树（过滤 build、.git、node_modules）
2) 解析代码并建立索引（先做轻量版）
   - 最稳方案：tree-sitter（跨语言强）
   - 备选：先只支持 C/C++/Python
3) 提供检索接口（JSON 输入输出）：
   - search_text(query, topk)
   - get_file(path)
   - get_symbols(file)
   - find_references(symbol)（后期再加）
验收标准：
- 对 1~5 万行小项目，索引 < 30s
- 检索能把相关文件 top3 找出来

## Milestone 3：Patch 系统（diff/应用/回滚）（3~5 天）
目标：保证“可控修改”（Cursor 核心之一）
1) 统一让 LLM 输出 unified diff 或“文件级替换块”
2) C++ 引擎实现：
   - apply_patch(diff)：成功/失败原因
   - snapshot()：修改前备份（hash + 备份目录）
   - rollback(snapshot_id)
验收标准：
- patch 失败必须给出明确原因（文件找不到/行号错/上下文不匹配）
- 一键回滚可恢复到修改前

## Milestone 4：Agent 工作流 v1（计划→检索→改代码）（1 周）
目标：跑通一条完整链路（固定工作流，便于答辩）
1) Plan：模型生成步骤计划（≤ 5 步）
2) Retrieve：根据 plan 生成检索 query → 调用引擎拿上下文
3) Propose Patch：模型只输出 diff（严格格式）
4) Apply：引擎应用 patch
5) Explain：模型解释改动（用于答辩/复盘）
验收标准（固定 3 个任务）：
- 修一个编译错误
- 给函数加参数校验
- 给项目加一个小功能（比如 CLI 参数）

## Milestone 5：运行与自我修复（编译/测试/重试）（1 周）
目标：让它“像工具”而不是“像聊天”
1) engine 提供 run(cmd, timeout) 返回：
   - exit code
   - stdout/stderr（截断）
2) agent 增加循环（最多重试 2 次，避免死循环）：
   - apply → run → 若失败：抽取 error → retrieve 相关文件 → 生成修复 patch → 再 run
3) 每次 run 的命令、输出、diff、检索上下文都必须落盘
验收标准：
- 失败时能自动修复至少 1 类典型问题（缺 include、函数签名不一致、未声明变量等）
- 日志可复现（能复跑同样命令与看到同样输出）

## Milestone 6：做成“Cursor 味”的交互（加分项，3~7 天）
按时间选一个即可：
- CLI 增强：输入需求 → 输出 plan / diff / 运行结果
- TUI（ncurses）：显示文件树、diff
- VSCode 插件（想冲可以做，但不建议一开始就做）

---

## 3. 关键工程细节（决定能不能稳答辩）

1) 上下文拼接策略（避免爆 token）
- 永远不要把整文件塞给模型
- 只给：相关函数 + 相关调用点 + 错误日志 + 文件路径

2) 变更必须“结构化”
- 模型输出：只准 diff（必要时外加 JSON 元信息）
- 不允许“建议你改一下……”但不给 diff 的输出

3) 日志与可复现（答辩救命）
必须落盘：
- 每次对话 prompt（可脱敏）
- 检索结果（哪些文件/片段）
- diff
- 编译/测试命令与输出

---

## 4. 毕设文档怎么写（顺手就能写出来）

- 需求分析：为什么需要 coding agent（效率、自动化）
- 系统设计：三层架构 + 工具接口协议（JSON）
- 核心模块：
  - 索引检索（C++）
  - patch/回滚（C++）
  - agent 编排（Python）
  - 失败自修复（循环策略）
- 实验与评估：
  - 任务成功率（3~10 个任务）
  - 平均迭代次数
  - 平均耗时
  - diff 行数、回滚次数
