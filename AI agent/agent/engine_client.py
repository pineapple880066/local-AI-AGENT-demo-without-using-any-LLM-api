"""
agent/engine_client.py：Python 侧对 C++ 引擎（engine_cli）的“薄封装”

为什么需要这个文件？
- 你希望 agent 编排层（Python）能调用本地能力（C++ 引擎）：
  例如 list_files / read_file / search_text / apply_patch / rollback 等。
- 最稳妥的方式是：Python 通过 subprocess 启动一个 CLI 程序，然后用 JSON 通信。
  这样避免了 pybind11/ffi 在毕设阶段容易卡住的工程复杂度，也更容易答辩解释。

EngineClient 的职责：
1) 统一构造 engine_cli 的命令行参数
2) 解析 engine_cli 输出的 JSON（stdout）
3) 把结果以 dict 返回给上层 workflow

注意：
- 当前 demo 的协议非常轻量（只为了跑通链路）。
- 真正项目里建议你把 JSON schema 固定下来，并对输入输出做更严格的校验。
"""

import json
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict


@dataclass(frozen=True)
class EngineClient:
    # engine_path：engine_cli 可执行文件的绝对路径
    engine_path: Path

    def _run(self, args: list[str]) -> Dict[str, Any]:
        """
        运行 engine_cli 并返回解析后的 JSON。

        约定：
        - engine_cli 正常情况下 stdout 输出一行 JSON
        - 退出码非 0 时，可能仍然会输出 JSON（包含 ok=false 与 error 字段）
        - 如果 stdout 不是合法 JSON，则认为引擎异常（engine_invalid_json）
        """
        proc = subprocess.run(
            [str(self.engine_path), *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if proc.returncode != 0 and not proc.stdout.strip():
            return {"ok": False, "error": "engine_failed", "stderr": proc.stderr, "args": args}
        try:
            payload = json.loads(proc.stdout)
        except json.JSONDecodeError:
            return {
                "ok": False,
                "error": "engine_invalid_json",
                "stdout": proc.stdout,
                "stderr": proc.stderr,
                "args": args,
            }
        if proc.returncode != 0 and payload.get("ok") is True:
            payload["ok"] = False
            payload["error"] = payload.get("error", "engine_nonzero_exit")
        return payload

    def list_files(self, root: Path) -> Dict[str, Any]:
        # 列出 root 下的文件树（会过滤掉常见的大目录，如 .git/node_modules 等）
        return self._run(["list-files", "--root", str(root)])

    def read_file(self, path: Path, max_bytes: int = 200_000) -> Dict[str, Any]:
        # 读取文件内容（max_bytes 用于控制上下文大小，避免一次读太大）
        return self._run(["read-file", "--path", str(path), "--max-bytes", str(max_bytes)])

    def search_text(
        self, root: Path, query: str, topk: int = 10, max_bytes: int = 200_000
    ) -> Dict[str, Any]:
        # 简单的全文搜索（demo 版本：逐文件逐行 find；后续可替换成倒排索引/rg/tree-sitter）
        return self._run(
            [
                "search-text",
                "--root",
                str(root),
                "--query",
                query,
                "--topk",
                str(topk),
                "--max-bytes",
                str(max_bytes),
            ]
        )

    def apply_edits(self, root: Path, edits_json_path: Path) -> Dict[str, Any]:
        # 应用“按行替换”的 edits.json，并自动做快照备份（root/.agent_snapshots/<id>/...）
        return self._run(
            ["apply-edits", "--root", str(root), "--edits-json", str(edits_json_path)]
        )

    def rollback(self, root: Path, snapshot_id: str) -> Dict[str, Any]:
        # 回滚到某次 apply_edits 之前的版本（把快照目录里的文件写回 root）
        return self._run(["rollback", "--root", str(root), "--snapshot-id", snapshot_id])
