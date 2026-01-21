"""
agent/cli.py：命令行入口（前端层的 CLI 版本）

你在终端里运行的：
  python3 -m agent.cli '修复示例项目编译错误' --workspace examples/demo_cpp

就是从这里开始。它的职责很薄：
1) 解析命令行参数
2) 组装 workspace / engine / logs 路径
3) 调用 workflow（真正逻辑在 agent/workflow.py）
4) 把结果以 JSON 打印出来，并用退出码表示成功/失败（便于脚本化）
"""

import argparse # argparse 用于解析命令行参数
import json # json 用于处理 JSON 数据
import sys # sys 用于访问与 Python 解释器相关的变量和函数
from pathlib import Path # pathlib 处理路径

from agent.engine_client import EngineClient # 引入 EngineClient 类, 用于与 C++ 引擎交互
from agent.workflow import run_workflow # 引入 run_workflow 函数, 用于执行工作流


def main() -> int:
    # prog="agent"：输出 help 时显示的程序名
    parser = argparse.ArgumentParser(prog="agent")

    # task：自然语言任务描述。当前 demo 里不会做复杂理解，主要用于日志记录/扩展点。
    parser.add_argument("task", help="Natural language task, e.g. 修复示例项目编译错误")

    # workspace：要被分析/修改/运行的项目根目录（示例是 examples/demo_cpp）
    parser.add_argument("--workspace", required=True, help="Workspace path (project to modify)")
    parser.add_argument(
        "--engine",
        default=str(Path(__file__).resolve().parents[1] / "engine" / "build" / "engine_cli"),
        help="Path to engine_cli executable",
    )
    parser.add_argument(
        "--logs",
        default=str(Path(".agent_logs").resolve()),
        help="Directory for run logs",
    )
    args = parser.parse_args()

    # resolve()：转成绝对路径，避免因为当前工作目录不同导致找不到文件
    workspace = Path(args.workspace).resolve()
    engine_path = Path(args.engine).resolve()
    logs_root = Path(args.logs).resolve()
    logs_root.mkdir(parents=True, exist_ok=True)

    # EngineClient：封装对 C++ 引擎 CLI 的调用（subprocess + JSON 解析）
    engine = EngineClient(engine_path=engine_path)

    # run_workflow：执行固定的 pipeline（Plan → Retrieve → Patch → Run → Fix）
    result = run_workflow(task=args.task, workspace=workspace, engine=engine, logs_root=logs_root)
    sys.stdout.write(json.dumps(result, ensure_ascii=False, indent=2) + "\n")

    # 退出码：0=成功；2=失败（便于脚本/CI 判断）
    return 0 if result.get("ok") else 2


if __name__ == "__main__":
    raise SystemExit(main())
