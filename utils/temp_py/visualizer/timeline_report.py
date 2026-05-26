#!/usr/bin/env python3
"""Generate a self-contained interactive HTML Gantt report from timeline JSON exports.

Reads:
  data/timeline/timeline_expNNN.json
  data/timeline/engagements_expNNN.json   (optional)
  data/timeline/phases_expNNN.json        (optional)

Writes:
  data/timeline/report_expNNN.html

The HTML pulls vis-timeline from a CDN; open it in a browser to drag/zoom.

Usage:
    python timeline_report.py --exp 57
    python timeline_report.py --timeline path/to/timeline.json
"""
from __future__ import annotations

import argparse
import html
import json
import os
import sys
from typing import Optional


def _platoon_of(actor: str) -> str:
    if not actor:
        return "(unknown)"
    if "-SOL" in actor:
        return actor.split("-SOL")[0]
    if "-LEADER" in actor:
        return actor.split("-LEADER")[0]
    if actor in ("BLUE-HQ", "RED-HQ"):
        return actor
    return actor


def _color_for_tag(tag: str) -> str:
    return {
        "FIRE": "#d33",
        "MOVE": "#3a78c2",
        "KIA": "#222",
        "PHASE_TRANSITION": "#e68a00",
    }.get(tag, "#777")


def build_html(
    timeline_path: str,
    engagements_path: Optional[str],
    phases_path: Optional[str],
    output_path: str,
) -> str:
    with open(timeline_path, "r", encoding="utf-8") as fh:
        tl = json.load(fh)

    phases = []
    if phases_path and os.path.isfile(phases_path):
        try:
            with open(phases_path, "r", encoding="utf-8") as fh:
                phases = (json.load(fh) or {}).get("phases", []) or []
        except Exception:
            phases = []

    engagements = []
    if engagements_path and os.path.isfile(engagements_path):
        try:
            with open(engagements_path, "r", encoding="utf-8") as fh:
                engagements = (json.load(fh) or {}).get("engagements", []) or []
        except Exception:
            engagements = []

    meta = tl.get("meta", {}) or {}
    raw_events = tl.get("events", []) or []

    items = []
    platoons = set()

    # vis-timeline expects ms epoch; we use sim seconds * 1000 directly
    for i, ev in enumerate(raw_events):
        actor = ev.get("actor") or ""
        tag = ev.get("tag", "")
        if not actor or not tag:
            continue
        group = _platoon_of(actor)
        platoons.add(group)
        t0 = float(ev.get("t0", 0.0))
        t1 = float(ev.get("t1", t0))
        if t1 <= t0:
            t1 = t0 + 0.5  # tiny width for instant events
        attrs = ev.get("attrs", {}) or {}
        attrs_str = " ".join(f"{k}={v}" for k, v in attrs.items())
        title = html.escape(f"{actor}  {tag}  t={t0:.1f}-{t1:.1f}s  {attrs_str}")
        colour = _color_for_tag(tag)
        items.append({
            "id": f"ev_{i}",
            "group": group,
            "start": t0 * 1000,
            "end": t1 * 1000,
            "type": "range",
            "content": tag,
            "style": f"background-color:{colour};color:white;border-color:{colour};",
            "title": title,
        })

    # Engagement lane
    for eg in engagements:
        title = html.escape(
            f"engagementId={eg.get('id','')}  fire={eg.get('fireCount',0)}  "
            f"blueKia={eg.get('blueKia',0)} redKia={eg.get('redKia',0)}  "
            f"blue={len(eg.get('blueIds',[]))} red={len(eg.get('redIds',[]))}"
        )
        items.append({
            "id": f"eg_{eg.get('id','')}",
            "group": "__ENGAGEMENT__",
            "start": float(eg.get("t0", 0.0)) * 1000,
            "end": float(eg.get("t1", eg.get("t0", 0.0))) * 1000,
            "type": "range",
            "content": (
                f"{eg.get('id','')} "
                f"(B{eg.get('blueKia',0)} / R{eg.get('redKia',0)})"
            ),
            "style": "background-color:#f5a623;color:#222;border-color:#c88517;",
            "title": title,
        })

    groups = [{"id": "__ENGAGEMENT__", "content": "Engagements", "order": -1}]
    for i, p in enumerate(sorted(platoons)):
        groups.append({"id": p, "content": p, "order": i})

    phase_regions = [{
        "phaseId": p.get("phaseId", ""),
        "t0": float(p.get("t0", 0.0)) * 1000,
        "t1": float(p.get("t1", 0.0)) * 1000,
        "triggerReason": p.get("triggerReason", ""),
    } for p in phases]

    html_doc = """<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>TDGsim Timeline Report - exp{exp}</title>
<script src="https://unpkg.com/vis-timeline@7.7.3/standalone/umd/vis-timeline-graph2d.min.js"></script>
<link href="https://unpkg.com/vis-timeline@7.7.3/styles/vis-timeline-graph2d.min.css" rel="stylesheet">
<style>
body {{ font-family: sans-serif; margin: 12px; background:#f5f5f5; color:#222; }}
h1 {{ font-size: 18px; margin: 0 0 4px 0; }}
.meta {{ color:#555; font-size:12px; margin-bottom:8px; }}
#timeline {{ background:white; border:1px solid #ccc; }}
.legend {{ margin: 6px 0 10px 0; }}
.legend span {{
  display:inline-block; padding:2px 10px; margin-right:6px;
  color:white; font-size:12px; border-radius:3px;
}}
.phase-strip {{
  display:flex; align-items:center; gap:6px; margin:6px 0 10px 0;
  font-size:12px; color:#333;
}}
.phase-strip .pill {{
  padding:2px 8px; border-radius:10px; background:#dde;
}}
</style>
</head>
<body>
<h1>TDGsim Timeline Report - Experiment {exp}</h1>
<div class="meta">
seed={seed} &nbsp; endTime={endT}s &nbsp; map={w}x{h}
&nbsp;|&nbsp; events={n_ev} &nbsp; engagements={n_eg} &nbsp; phases={n_ph}
</div>
<div class="legend">
<span style="background:#d33">FIRE</span>
<span style="background:#3a78c2">MOVE</span>
<span style="background:#222">KIA</span>
<span style="background:#f5a623;color:#222">ENGAGEMENT</span>
<span style="background:#e68a00">PHASE_TRANSITION</span>
</div>
<div class="phase-strip">
<strong>Phases:</strong>
{phase_pills}
</div>
<div id="timeline" style="width:100%; height:80vh"></div>
<script>
const items = new vis.DataSet({items_json});
const groups = new vis.DataSet({groups_json});
const phaseRegions = {phases_json};

const options = {{
  stack: true,
  zoomMin: 500,
  zoomMax: 1000 * 60 * 60 * 2,
  margin: {{ item: 3 }},
  showCurrentTime: false,
  orientation: 'top',
  groupOrder: 'order',
  format: {{
    minorLabels: {{
      millisecond: 'S[ms]', second: 's[s]',
      minute: 'm:ss', hour: 'H:mm:ss'
    }},
    majorLabels: {{ second: '', minute: '', hour: '' }}
  }}
}};

const container = document.getElementById('timeline');
const timeline = new vis.Timeline(container, items, groups, options);

const phaseColors = ['rgba( 80,160,220,0.18)',
                     'rgba(220,160, 80,0.18)',
                     'rgba(140,200,120,0.18)',
                     'rgba(200,120,200,0.18)',
                     'rgba(120,200,200,0.18)'];
phaseRegions.forEach((p, idx) => {{
  items.add({{
    id: 'phaseBg_' + idx,
    start: p.t0, end: p.t1,
    type: 'background',
    content: p.phaseId + (p.triggerReason ? ' [' + p.triggerReason + ']' : ''),
    style: 'background-color:' + phaseColors[idx % phaseColors.length] + ';',
  }});
}});
</script>
</body>
</html>
"""

    phase_pills = "".join(
        f'<span class="pill">{html.escape(p.get("phaseId",""))}'
        f' @ {float(p.get("t0",0)):.0f}-{float(p.get("t1",0)):.0f}s'
        + (f' <em>({html.escape(p.get("triggerReason",""))})</em>'
           if p.get("triggerReason") else "")
        + "</span>"
        for p in phases
    ) or '<span class="pill">(none)</span>'

    out = html_doc.format(
        exp=meta.get("experimentIndex", "?"),
        seed=meta.get("seed", "?"),
        endT=meta.get("endTime", "?"),
        w=meta.get("width", "?"),
        h=meta.get("height", "?"),
        n_ev=len(raw_events),
        n_eg=len(engagements),
        n_ph=len(phases),
        phase_pills=phase_pills,
        items_json=json.dumps(items),
        groups_json=json.dumps(groups),
        phases_json=json.dumps(phase_regions),
    )

    with open(output_path, "w", encoding="utf-8") as fh:
        fh.write(out)
    return output_path


def _repo_root_from(script: str) -> str:
    cur = os.path.abspath(os.path.dirname(script))
    while True:
        if os.path.isdir(os.path.join(cur, ".git")):
            return cur
        parent = os.path.dirname(cur)
        if parent == cur:
            return os.path.dirname(os.path.abspath(script))
        cur = parent


def _resolve_timeline(arg: Optional[str], exp: Optional[int], repo_root: str) -> Optional[str]:
    if arg:
        if os.path.isfile(arg):
            return os.path.abspath(arg)
        cand = os.path.join(repo_root, arg)
        if os.path.isfile(cand):
            return cand
        return None
    if exp is not None:
        return os.path.join(repo_root, "data", "timeline",
                            f"timeline_exp{int(exp):03d}.json")
    return None


def _sibling(timeline_path: str, prefix_new: str, prefix_old: str = "timeline_exp") -> str:
    base = os.path.basename(timeline_path)
    folder = os.path.dirname(timeline_path)
    if base.startswith(prefix_old):
        tail = base[len(prefix_old):]
        return os.path.join(folder, prefix_new + tail)
    return ""


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build a self-contained HTML Gantt report from TDGsim timeline JSON files."
    )
    parser.add_argument("--exp", type=int, help="Experiment index (e.g. 57).")
    parser.add_argument("--timeline", help="Explicit path to timeline_expNNN.json.")
    parser.add_argument("--out", help="Output HTML path (default: report_expNNN.html beside timeline).")
    args = parser.parse_args()

    repo_root = _repo_root_from(__file__)

    tl_path = _resolve_timeline(args.timeline, args.exp, repo_root)
    if not tl_path or not os.path.isfile(tl_path):
        sys.stderr.write("Timeline JSON not found. Pass --exp NNN or --timeline path.\n")
        sys.exit(2)

    eng_path = _sibling(tl_path, "engagements_exp")
    ph_path = _sibling(tl_path, "phases_exp")

    if args.out:
        out_path = args.out
    else:
        base = os.path.basename(tl_path)
        if base.startswith("timeline_exp"):
            out_name = base.replace("timeline_exp", "report_exp").replace(".json", ".html")
        else:
            out_name = "report.html"
        out_path = os.path.join(os.path.dirname(tl_path), out_name)

    build_html(tl_path, eng_path or None, ph_path or None, out_path)
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()
