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
SOLDIER_DEAD_RE = re.compile(
    r"\]\s+(?P<leader>(?:BLUE|RED)-PLT\d+)-SOL\d+-MNV\s+:\s+DEAD"
)
PENDING_RE = re.compile(
    r"\]\s+(?P<leader>(?:BLUE|RED)-PLT\d+)-LEADER\([^)]*\)\s+:\s+MOVE_GOAL_PENDING\s+"
    r"(?P<detail>.*)"
)
PENDING_DETAIL_RE = re.compile(
    r"reached=(?P<reached>\d+)/(?P<alive>\d+)\s+required=(?P<required>\d+)\s+"
    r"centroid=\((?P<cx>-?\d+),(?P<cy>-?\d+)\)\s+"
    r"goal=\((?P<gx>-?\d+),(?P<gy>-?\d+)\)\s+"
    r"centroidDist=(?P<dist>-?\d+)"
)
TO_RE = re.compile(r"\bto=(?P<x>-?\d+),(?P<y>-?\d+)")
GOAL_RE = re.compile(r"\bgoal=\((?P<x>-?\d+),(?P<y>-?\d+)\)")
MEMBERS_RE = re.compile(r"\bmembers=(?P<members>\d+)")


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


def parse_pending_detail(detail: str) -> dict[str, object]:
    parsed: dict[str, object] = {"raw": detail.strip()}
    match = PENDING_DETAIL_RE.search(detail)
    if not match:
        return parsed

    parsed.update(
        {
            "reached": int(match.group("reached")),
            "alive": int(match.group("alive")),
            "required": int(match.group("required")),
            "centroid": f"{match.group('cx')},{match.group('cy')}",
            "goal": f"{match.group('gx')},{match.group('gy')}",
            "centroid_dist": int(match.group("dist")),
        }
    )
    sample = detail.split("sample_unreached=", 1)
    if len(sample) == 2:
        parsed["sample_unreached"] = sample[1].strip()
    return parsed


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
    run_events = defaultdict(lambda: defaultdict(Counter))
    deaths_by_run = defaultdict(Counter)
    pending_by_run = defaultdict(dict)
    platoon_size_hint = defaultdict(int)

    for path in files:
        run_name = path.name
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for line in f:
                dead_match = SOLDIER_DEAD_RE.search(line)
                if dead_match:
                    leader = dead_match.group("leader")
                    stats[leader]["DEAD"] += 1
                    deaths_by_run[leader][run_name] += 1
                    run_events[leader][run_name]["DEAD"] += 1
                    continue

                pending_match = PENDING_RE.search(line)
                if pending_match:
                    leader = pending_match.group("leader")
                    pending = parse_pending_detail(pending_match.group("detail"))
                    pending_by_run[leader][run_name] = pending
                    alive = pending.get("alive")
                    if isinstance(alive, int):
                        platoon_size_hint[leader] = max(platoon_size_hint[leader], alive)
                    continue

                event_match = LEADER_EVENT_RE.search(line)
                if not event_match:
                    continue

                leader = event_match.group("leader")
                event = event_match.group("event")

                members_match = MEMBERS_RE.search(line)
                if members_match:
                    platoon_size_hint[leader] = max(
                        platoon_size_hint[leader], int(members_match.group("members"))
                    )

                if event == "ACTIVATE_ORDER":
                    if "task=MOVE" not in line:
                        continue
                    stats[leader]["ACTIVATE_MOVE"] += 1
                    run_events[leader][run_name]["ACTIVATE_MOVE"] += 1
                    to_match = TO_RE.search(line)
                    if to_match:
                        activate_targets[leader][coord_from(to_match)] += 1
                    continue

                stats[leader][event] += 1
                run_events[leader][run_name][event] += 1

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
        "run_events": run_events,
        "deaths_by_run": deaths_by_run,
        "pending_by_run": pending_by_run,
        "platoon_size_hint": platoon_size_hint,
    }


def top_counter(counter: Counter, limit: int = 3) -> str:
    if not counter:
        return "없음"
    return ", ".join(f"{key}: {value}회" for key, value in counter.most_common(limit))


def leader_comment(
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
        base = "대부분의 MOVE는 도달하지만 일부 목표에서 재경로 후에도 실패합니다."
    elif reached > 0:
        base = "일부 MOVE는 도달하지만 타임아웃으로 다음 명령에 넘어가는 비율이 큽니다."
    else:
        base = "MOVE_GOAL_REACHED가 없어 해당 MOVE 축선은 목표 도달에 실패하고 있습니다."

    if target:
        if target_count >= run_count:
            return f"{base} {target} 이동은 {target_count}회 모두 재경로 후 MOVE_TIMEOUT_ADVANCE로 넘어갑니다."
        return f"{base} 문제는 주로 {target} 목표입니다({target_count}회)."
    return base


def death_summary(
    files: list[Path],
    deaths: Counter,
    platoon_size: int,
) -> str:
    counts = [deaths.get(path.name, 0) for path in files]
    total = sum(counts)
    if total == 0:
        return "사망 로그: 0회"

    runs_with_death = sum(1 for count in counts if count > 0)
    wipeouts = sum(1 for count in counts if platoon_size > 0 and count >= platoon_size)
    avg = total / len(files)
    parts = [
        f"사망 로그: {total}회",
        f"사망 발생 실행: {runs_with_death}/{len(files)}회",
        f"실행당 평균/최대 사망: {avg:.2f}/{max(counts)}명",
    ]
    if wipeouts:
        parts.append(f"전멸 추정 실행: {wipeouts}회")
    return ", ".join(parts)


def format_pending_sample(run_name: str, pending: dict[str, object]) -> str:
    if not pending:
        return "없음"

    reached = pending.get("reached")
    alive = pending.get("alive")
    required = pending.get("required")
    centroid = pending.get("centroid")
    goal = pending.get("goal")
    dist = pending.get("centroid_dist")
    sample = str(pending.get("sample_unreached", "")).strip()
    if len(sample) > 180:
        sample = sample[:177] + "..."

    if all(value is not None for value in [reached, alive, required, centroid, goal, dist]):
        base = (
            f"{run_name}: reached={reached}/{alive}, required={required}, "
            f"centroid=({centroid}), goal=({goal}), centroidDist={dist}"
        )
        if sample:
            base += f", sample_unreached={sample}"
        return base

    raw = str(pending.get("raw", "")).strip()
    if len(raw) > 220:
        raw = raw[:217] + "..."
    return f"{run_name}: {raw}"


def build_failure_trace(
    leader: str,
    files: list[Path],
    run_events: dict,
    deaths_by_run: dict,
    pending_by_run: dict,
    platoon_size: int,
) -> list[str]:
    traces: list[str] = []
    advance_runs = [
        path.name
        for path in files
        if run_events[leader][path.name].get("MOVE_TIMEOUT_ADVANCE", 0) > 0
    ]
    if not advance_runs:
        return traces

    no_death_advances = sum(1 for run in advance_runs if deaths_by_run[leader].get(run, 0) == 0)
    death_advances = len(advance_runs) - no_death_advances
    wipeout_without_advance = sum(
        1
        for path in files
        if run_events[leader][path.name].get("ACTIVATE_MOVE", 0) > 0
        and run_events[leader][path.name].get("MOVE_GOAL_REACHED", 0) == 0
        and run_events[leader][path.name].get("MOVE_TIMEOUT_ADVANCE", 0) == 0
        and platoon_size > 0
        and deaths_by_run[leader].get(path.name, 0) >= platoon_size
    )

    traces.append(
        "미도달 추적: "
        f"MOVE_TIMEOUT_ADVANCE 실행 {len(advance_runs)}회 중 "
        f"사망 없음 {no_death_advances}회, 사망 있음 {death_advances}회"
    )
    if wipeout_without_advance:
        traces.append(
            f"미도달 추적: 전멸로 MOVE_TIMEOUT_ADVANCE까지 가지 못한 실행 {wipeout_without_advance}회"
        )

    sample_run = next(
        (run for run in advance_runs if run in pending_by_run[leader]),
        None,
    )
    if sample_run:
        traces.append(
            "마지막 미도달 샘플: "
            + format_pending_sample(sample_run, pending_by_run[leader][sample_run])
        )

    return traces


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
    run_events = analysis["run_events"]
    deaths_by_run = analysis["deaths_by_run"]
    pending_by_run = analysis["pending_by_run"]
    platoon_size_hint = analysis["platoon_size_hint"]
    run_count = len(files)

    lines: list[str] = []
    lines.append(f"분석 대상 로그: {run_count}개")
    lines.append(f"범위: {files[0].name} ~ {files[-1].name}")
    lines.append("")
    lines.append("[MOVE 상태 요약]")
    lines.append("")

    for leader in leaders:
        stats = stats_by_leader[leader]
        platoon_size = platoon_size_hint.get(leader, 0)
        lines.append(leader)
        lines.append("")
        lines.append(f"ACTIVATE_ORDER(MOVE): {stats.get('ACTIVATE_MOVE', 0)}")
        lines.append(f"MOVE_GOAL_REACHED: {stats.get('MOVE_GOAL_REACHED', 0)}")
        lines.append(f"MOVE_REPLAN: {stats.get('MOVE_REPLAN', 0)}")
        lines.append(f"MOVE_TIMEOUT: {stats.get('MOVE_TIMEOUT', 0)}")
        lines.append(f"MOVE_TIMEOUT_ADVANCE: {stats.get('MOVE_TIMEOUT_ADVANCE', 0)}")
        lines.append(f"DEAD: {stats.get('DEAD', 0)}")
        if order_failed[leader]:
            lines.append(f"ORDER_FAILED/SKIP_ORDER: {top_counter(order_failed[leader])}")
        lines.append(leader_comment(run_count, stats, timeout_targets[leader]))
        lines.append(death_summary(files, deaths_by_run[leader], platoon_size))
        for trace_line in build_failure_trace(
            leader,
            files,
            run_events,
            deaths_by_run,
            pending_by_run,
            platoon_size,
        ):
            lines.append(trace_line)
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
