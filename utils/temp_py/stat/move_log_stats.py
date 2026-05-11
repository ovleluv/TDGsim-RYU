import argparse
import re
from collections import Counter, defaultdict
from pathlib import Path


DEFAULT_LOG_DIR = "logs"
DEFAULT_OUTPUT = "data/move_log_stats_summary.txt"

LEADER_EVENT_RE = re.compile(
    r"\]\s+(?P<leader>(?:BLUE|RED)-PLT\d+)-LEADER\([^)]*\)\s+:\s+"
    r"(?P<event>ACTIVATE_ORDER|MOVE_GOAL_REACHED|MOVE_REPLAN|MOVE_TIMEOUT_ADVANCE|MOVE_TIMEOUT|ORDER_FAILED|SKIP_ORDER)"
)
TO_RE = re.compile(r"\bto=(?P<x>-?\d+),(?P<y>-?\d+)")
GOAL_RE = re.compile(r"\bgoal=\((?P<x>-?\d+),(?P<y>-?\d+)\)")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Analyze MOVE progress events from TDG simulation logs."
    )
    parser.add_argument(
        "log_dir",
        nargs="?",
        default=DEFAULT_LOG_DIR,
        help=f"Directory containing log_simulation_exp*.txt (default: {DEFAULT_LOG_DIR})",
    )
    parser.add_argument(
        "-o",
        "--output",
        default=DEFAULT_OUTPUT,
        help=f"Where to write the text summary (default: {DEFAULT_OUTPUT})",
    )
    return parser


def run_number(path: Path) -> int:
    match = re.search(r"exp(\d+)", path.stem)
    return int(match.group(1)) if match else 10**9


def leader_sort_key(leader: str) -> tuple[int, int, str]:
    side, platoon = leader.split("-PLT", 1)
    side_order = 0 if side == "BLUE" else 1
    try:
        number = int(platoon)
    except ValueError:
        number = 10**9
    return side_order, number, leader


def coord_from(match: re.Match[str]) -> str:
    return f"{match.group('x')},{match.group('y')}"


def analyze_logs(log_dir: Path) -> dict:
    files = sorted(log_dir.glob("log_simulation_exp*.txt"), key=run_number)
    if not files:
        raise FileNotFoundError(f"No log_simulation_exp*.txt files found in {log_dir}")

    stats = defaultdict(Counter)
    activate_targets = defaultdict(Counter)
    reached_targets = defaultdict(Counter)
    replan_targets = defaultdict(Counter)
    timeout_advance_targets = defaultdict(Counter)
    timeout_advance_reasons = defaultdict(Counter)
    order_failed = defaultdict(Counter)

    for path in files:
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                event_match = LEADER_EVENT_RE.search(line)
                if not event_match:
                    continue

                leader = event_match.group("leader")
                event = event_match.group("event")

                if event == "ACTIVATE_ORDER":
                    if "task=MOVE" not in line:
                        continue
                    stats[leader]["ACTIVATE_MOVE"] += 1
                    to_match = TO_RE.search(line)
                    if to_match:
                        activate_targets[leader][coord_from(to_match)] += 1
                    continue

                stats[leader][event] += 1

                if event == "MOVE_GOAL_REACHED":
                    goal_match = GOAL_RE.search(line)
                    if goal_match:
                        reached_targets[leader][coord_from(goal_match)] += 1
                elif event == "MOVE_REPLAN":
                    to_match = TO_RE.search(line)
                    if to_match:
                        replan_targets[leader][coord_from(to_match)] += 1
                elif event == "MOVE_TIMEOUT_ADVANCE":
                    to_match = TO_RE.search(line)
                    if to_match:
                        timeout_advance_targets[leader][coord_from(to_match)] += 1
                    reason_match = re.search(r"\breason=([^\s]+)", line)
                    if reason_match:
                        timeout_advance_reasons[leader][reason_match.group(1)] += 1
                elif event in {"ORDER_FAILED", "SKIP_ORDER"}:
                    order_failed[leader][event] += 1

    leaders = sorted(stats.keys(), key=leader_sort_key)
    return {
        "files": files,
        "leaders": leaders,
        "stats": stats,
        "activate_targets": activate_targets,
        "reached_targets": reached_targets,
        "replan_targets": replan_targets,
        "timeout_advance_targets": timeout_advance_targets,
        "timeout_advance_reasons": timeout_advance_reasons,
        "order_failed": order_failed,
    }


def top_counter(counter: Counter, limit: int = 3) -> str:
    if not counter:
        return "없음"
    return ", ".join(f"{key}: {value}회" for key, value in counter.most_common(limit))


def leader_comment(
    leader: str,
    run_count: int,
    stats: Counter,
    timeout_targets: Counter,
) -> str:
    activated = stats.get("ACTIVATE_MOVE", 0)
    reached = stats.get("MOVE_GOAL_REACHED", 0)
    advanced = stats.get("MOVE_TIMEOUT_ADVANCE", 0)

    if activated <= 0:
        return "MOVE 명령이 활성화되지 않았습니다."

    if advanced == 0:
        return "모든 MOVE가 최종 타임아웃 없이 처리되었습니다."

    target, target_count = ("", 0)
    if timeout_targets:
        target, target_count = timeout_targets.most_common(1)[0]

    if reached >= activated * 0.9:
        base = "거의 모든 MOVE가 목표 도달 판정까지 갑니다."
    elif reached >= activated * 0.6:
        base = "대부분의 MOVE는 도달하지만, 일부 목표에서 재경로 후에도 실패합니다."
    elif reached > 0:
        base = "일부 MOVE는 도달하지만, 타임아웃 후 다음 명령으로 넘어가는 비율이 큽니다."
    else:
        base = "MOVE_GOAL_REACHED가 없어 해당 MOVE 축선은 목표 도달에 실패하고 있습니다."

    if target:
        if target_count >= run_count:
            return f"{base} {target} 이동은 {target_count}회 모두 재경로 후에도 MOVE_TIMEOUT_ADVANCE로 넘어갑니다."
        return f"{base} 문제는 주로 {target} 목표입니다({target_count}회)."
    return base


def build_report(analysis: dict) -> str:
    files = analysis["files"]
    leaders = analysis["leaders"]
    stats_by_leader = analysis["stats"]
    activate_targets = analysis["activate_targets"]
    reached_targets = analysis["reached_targets"]
    replan_targets = analysis["replan_targets"]
    timeout_targets = analysis["timeout_advance_targets"]
    timeout_reasons = analysis["timeout_advance_reasons"]
    order_failed = analysis["order_failed"]
    run_count = len(files)

    lines: list[str] = []
    lines.append(f"분석 대상 로그: {run_count}개")
    lines.append(f"범위: {files[0].name} ~ {files[-1].name}")
    lines.append("")
    lines.append("[MOVE 상태 요약]")
    lines.append("")

    for leader in leaders:
        stats = stats_by_leader[leader]
        lines.append(leader)
        lines.append("")
        lines.append(f"ACTIVATE_ORDER(MOVE): {stats.get('ACTIVATE_MOVE', 0)}")
        lines.append(f"MOVE_GOAL_REACHED: {stats.get('MOVE_GOAL_REACHED', 0)}")
        lines.append(f"MOVE_REPLAN: {stats.get('MOVE_REPLAN', 0)}")
        lines.append(f"MOVE_TIMEOUT: {stats.get('MOVE_TIMEOUT', 0)}")
        lines.append(f"MOVE_TIMEOUT_ADVANCE: {stats.get('MOVE_TIMEOUT_ADVANCE', 0)}")
        if order_failed[leader]:
            lines.append(f"ORDER_FAILED/SKIP_ORDER: {top_counter(order_failed[leader])}")
        lines.append(leader_comment(leader, run_count, stats, timeout_targets[leader]))
        lines.append(f"활성화 좌표: {top_counter(activate_targets[leader], limit=10)}")
        lines.append(f"도달 좌표: {top_counter(reached_targets[leader], limit=10)}")
        lines.append(f"재경로 좌표: {top_counter(replan_targets[leader], limit=10)}")
        lines.append("")

    lines.append("좌표별 MOVE_TIMEOUT_ADVANCE:")
    lines.append("")
    any_timeout = False
    for leader in leaders:
        for target, count in timeout_targets[leader].most_common():
            any_timeout = True
            lines.append(f"{leader} -> {target}: {count}회")
    if not any_timeout:
        lines.append("없음")

    lines.append("")
    lines.append("MOVE_TIMEOUT_ADVANCE 사유:")
    lines.append("")
    any_reason = False
    for leader in leaders:
        if timeout_reasons[leader]:
            any_reason = True
            lines.append(f"{leader}: {top_counter(timeout_reasons[leader], limit=10)}")
    if not any_reason:
        lines.append("없음")

    return "\n".join(lines)


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    log_dir = Path(args.log_dir)
    output_path = Path(args.output)

    report = build_report(analyze_logs(log_dir))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(report + "\n", encoding="utf-8")

    print(report)
    print(f"\nSummary saved to: {output_path}")


if __name__ == "__main__":
    main()
