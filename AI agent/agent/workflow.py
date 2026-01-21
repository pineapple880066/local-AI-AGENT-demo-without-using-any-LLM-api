"""
agent/workflow.py：一个“最小可跑通”的端到端工作流（demo 版）

你可以把它当成 Cursor/Claude Code/各种 coding-agent 的核心骨架：
  Plan → Retrieve → Patch → Run → Fix

但为了让你一个人也能在短时间内跑通毕设，这里刻意做了简化：
- Plan：固定模板（不是 LLM 生成）
- Retrieve：用 engine_cli 做“读文件/搜索文本”拿上下文
- Patch：用 engine_cli 的 apply-edits 做“按行替换”，并自动生成 snapshot（可回滚）
- Run：调用 workspace 下的 ./build.sh（由例子项目提供）
- Fix：只实现一个类别：根据编译错误推断缺少的 #include（thread/chrono）

后续扩展思路：
- 你可以把 _plan() 替换成 LLM 生成计划（≤5步）。
- 你可以把“推断缺 include”替换成 LLM 生成 unified diff，再交给 C++ patch 引擎应用。
- 你可以把 Run/Fix 做成循环：失败→提取错误→检索上下文→生成修复 patch→再 run（最多 2 次）。
"""

import json
import re
import subprocess
from pathlib import Path
from typing import Any, Dict, List

from agent.engine_client import EngineClient


def _run_cmd(cmd: List[str], cwd: Path, timeout_s: int = 30) -> Dict[str, Any]:
    """
    运行一个外部命令，并把结果结构化返回。

    为什么要结构化？
    - 方便日志落盘、复现、答辩展示（cmd/exit code/stdout/stderr 一目了然）
    - 未来接 LLM 时，也可以把 stderr 的关键部分喂给模型做修复
    """
    proc = subprocess.run(
        cmd,
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout_s,
    )
    return {"code": proc.returncode, "stdout": proc.stdout, "stderr": proc.stderr, "cmd": cmd}


def _extract_missing_includes(build_stderr: str) -> List[str]:
    """
    从编译错误 stderr 里“猜”出可能缺少的标准库头文件。

    这是 demo 里用于跑通闭环的规则策略：
    - 看到 std::this_thread 相关报错 → 推断需要 #include <thread>
    - 看到 std::chrono 相关报错 → 推断需要 #include <chrono>

    真正毕设版本你可以：
    - 扩展更多规则（比如 vector/string/optional 等）
    - 或直接把 build_stderr + 相关代码片段交给 LLM 生成 patch
    """
    need: List[str] = []

    def add(header: str) -> None:
        if header not in need:
            need.append(header)

    if re.search(r"(no member named|is not a member of)\s+'chrono'\s+in\s+namespace\s+'std'", build_stderr) or re.search(
        r"error: 'chrono' is not a member of 'std'", build_stderr
    ):
        add("chrono")

    if re.search(r"(no member named|is not a member of)\s+'this_thread'\s+in\s+namespace\s+'std'", build_stderr) or re.search(
        r"error: 'this_thread' is not a member of 'std'", build_stderr
    ):
        add("thread")

    if re.search(r"namespace\s+'std::this_thread'", build_stderr) or re.search(
        r"namespace\s+‘std::this_thread’", build_stderr
    ):
        add("thread")

    return need


def _plan(task: str) -> List[str]:
    """
    Plan 阶段：当前 demo 直接返回固定计划。

    为啥先固定？
    - 你还在搭骨架时，“可控、可复现”比“聪明”更重要
    - 答辩时也更容易解释每一步在做什么
    """
    return [
        "运行示例项目的 build，拿到错误日志",
        "从错误里提取关键词，用引擎检索相关文件与上下文",
        "生成最小修改（edits/diff），并应用修改",
        "再次运行 build 验证修复结果",
    ]


def run_workflow(task: str, workspace: Path, engine: EngineClient, logs_root: Path) -> Dict[str, Any]:
    """
    执行一次完整 workflow，并把过程落盘到 logs_root/<run_id>/。

    你可以把 run_id 当成一次“可复现的实验编号”：
    - task.txt：用户需求
    - plan.json：计划
    - build_0.json：第一次 build 输出（通常会失败）
    - retrieve.json：检索结果（demo 里只是示意）
    - edits.json：将要应用的修改
    - apply.json：引擎应用结果（含 snapshot_id）
    - build_1.json：第二次 build 输出（希望成功）
    """
    run_id = str(int(__import__("time").time() * 1000))
    run_dir = logs_root / run_id
    run_dir.mkdir(parents=True, exist_ok=True)

    # 1) 记录任务与计划（这两份是答辩最需要展示的“流程证据”）
    (run_dir / "task.txt").write_text(task, encoding="utf-8")
    plan = _plan(task)
    (run_dir / "plan.json").write_text(json.dumps(plan, ensure_ascii=False, indent=2), encoding="utf-8")

    # 2) Run：运行示例项目自己的 build.sh（第一次通常会失败，产生编译错误日志）
    build = _run_cmd(["./build.sh"], cwd=workspace, timeout_s=60)
    (run_dir / "build_0.json").write_text(json.dumps(build, ensure_ascii=False, indent=2), encoding="utf-8")
    if build["code"] == 0:
        return {"ok": True, "run_id": run_id, "message": "build already OK"}

    # 3) Fix(规则)：从 stderr 推断需要补哪些 include（demo 版本只处理 thread/chrono）
    needed_headers = _extract_missing_includes(build["stderr"])
    if not needed_headers:
        return {"ok": False, "run_id": run_id, "error": "unsupported_build_error", "build": build}

    # 4) Retrieve：示意性调用一下搜索接口（真实版本应该用“错误关键词/符号名”去检索）
    retrieve = {"search": engine.search_text(root=workspace, query="std::", topk=5)}
    (run_dir / "retrieve.json").write_text(
        json.dumps(retrieve, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    # 5) 读取目标文件内容（demo 里固定是 main.cpp；真实版本应由检索/计划决定目标文件）
    target_path = workspace / "main.cpp"
    file_payload = engine.read_file(target_path)
    if not file_payload.get("ok"):
        return {"ok": False, "run_id": run_id, "error": "read_file_failed", "detail": file_payload}

    content = file_payload["content"]
    # 某些编译器/情况下，stderr 只报第一个错误。
    # 所以我们也从代码里做一次补充推断：出现 std::chrono:: / std::this_thread:: 但没 include 就加上。
    if "std::chrono::" in content and "#include <chrono>" not in content and "chrono" not in needed_headers:
        needed_headers.append("chrono")
    if "std::this_thread::" in content and "#include <thread>" not in content and "thread" not in needed_headers:
        needed_headers.append("thread")

    needed_headers = [h for h in needed_headers if f"#include <{h}>" not in content]
    if not needed_headers:
        return {"ok": False, "run_id": run_id, "error": "includes_already_present"}

    # 6) Patch（最小改动）：把 include 插入到最后一个 #include 之后
    lines = content.splitlines()
    insert_at = 0
    for i, line in enumerate(lines):
        if line.startswith("#include"):
            insert_at = i + 1
    new_lines = lines[:insert_at] + [f"#include <{h}>" for h in needed_headers] + lines[insert_at:]
    replacement = "\n".join(new_lines) + ("\n" if content.endswith("\n") else "")

    # 7) 把修改写成 edits.json（engine_cli 的 apply-edits 只认识这种格式）
    edits = {
        "edits": [
            {
                "path": "main.cpp",
                "start_line": 1,
                "end_line": len(lines),
                "replacement": replacement,
            }
        ]
    }
    edits_path = run_dir / "edits.json"
    edits_path.write_text(json.dumps(edits, ensure_ascii=False, indent=2), encoding="utf-8")

    # 8) Apply：调用引擎应用修改；引擎会在 workspace/.agent_snapshots 下生成快照
    apply_res = engine.apply_edits(root=workspace, edits_json_path=edits_path)
    (run_dir / "apply.json").write_text(
        json.dumps(apply_res, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    if not apply_res.get("ok"):
        return {"ok": False, "run_id": run_id, "error": "apply_failed", "detail": apply_res}

    # 9) 再次运行 build 验证修复是否成功
    build2 = _run_cmd(["./build.sh"], cwd=workspace, timeout_s=60)
    (run_dir / "build_1.json").write_text(
        json.dumps(build2, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    if build2["code"] != 0:
        return {"ok": False, "run_id": run_id, "error": "still_failing", "build": build2}

    return {
        "ok": True,
        "run_id": run_id,
        "fixed": "missing_include",
        "added_includes": needed_headers,
        "snapshot_id": apply_res.get("snapshot_id"),
    }
