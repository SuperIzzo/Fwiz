#!/usr/bin/env python3
"""
session-stats.py — Compute context-proxy stats from .fwiz-workflow/orchestrator-log.md
and the artifact spread in .fwiz-workflow/.

Used by the log-arc reflector (and any other agent that wants to judge "context
state") without direct visibility into Claude's actual context window. The agent
can't see token count; this script gives it observable proxies.

Usage:
  ./tools/session-stats.py             # human-readable
  ./tools/session-stats.py --json      # JSON output
  ./tools/session-stats.py --lookback-hours 12

Session boundary precedence:
  1. Last "## CONTEXT-CLEARED" or "## SESSION-START" marker in orchestrator-log.md.
  2. Otherwise, entries within the last `--lookback-hours` (default 6).
  3. Otherwise, all parseable entries.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORKFLOW = ROOT / ".fwiz-workflow"
ORCH_LOG = WORKFLOW / "orchestrator-log.md"

# Match `## [TIMESTAMP] LABEL` or `### [TIMESTAMP] LABEL` (orchestrator profile
# specifies ###; actual log uses ##; accept both).
ENTRY_RE = re.compile(r"^#{2,4}\s+\[([^\]]+)\]\s+(.+?)\s*$")
BOUNDARY_RE = re.compile(r"^#{2,4}\s+(CONTEXT-CLEARED|SESSION-START)\b")

DEFAULT_LOOKBACK_HOURS = 6


def parse_timestamp(raw: str) -> datetime | None:
    """Accept ISO-8601 or space-separated; return UTC-aware datetime or None."""
    s = raw.strip().replace(" ", "T")
    if s.endswith("Z"):
        s = s[:-1] + "+00:00"
    try:
        dt = datetime.fromisoformat(s)
    except ValueError:
        return None
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    return dt


def find_last_boundary(lines: list[str]) -> int | None:
    """Index (line number) of last session boundary marker, or None."""
    last = None
    for i, line in enumerate(lines):
        if BOUNDARY_RE.match(line):
            last = i
    return last


def parse_entries(lines: list[str]) -> list[tuple[datetime, str, int]]:
    """List of (timestamp, label, line_index) for every parseable entry."""
    out = []
    for i, line in enumerate(lines):
        m = ENTRY_RE.match(line)
        if not m:
            continue
        ts = parse_timestamp(m.group(1))
        if ts is None:
            continue
        out.append((ts, m.group(2), i))
    return out


def compute_log_stats(text: str, lookback_hours: int) -> dict:
    lines = text.splitlines()
    boundary = find_last_boundary(lines)
    entries = parse_entries(lines)

    if not entries:
        return {
            "session_start": None,
            "session_end": None,
            "wall_time_minutes": 0,
            "agent_spawns": 0,
            "phase_or_cycle_events": 0,
            "total_entries": 0,
            "log_lines_since_session": 0,
            "log_bytes_since_session": 0,
            "boundary": "no-entries",
        }

    if boundary is not None:
        in_session = [e for e in entries if e[2] > boundary]
        boundary_kind = "explicit-marker"
    else:
        cutoff = datetime.now(timezone.utc) - timedelta(hours=lookback_hours)
        in_session = [e for e in entries if e[0] >= cutoff]
        boundary_kind = f"lookback-{lookback_hours}h"
        if not in_session:
            in_session = entries
            boundary_kind = "all-entries"

    if not in_session:
        return {
            "session_start": None,
            "session_end": None,
            "wall_time_minutes": 0,
            "agent_spawns": 0,
            "phase_or_cycle_events": 0,
            "total_entries": 0,
            "log_lines_since_session": 0,
            "log_bytes_since_session": 0,
            "boundary": boundary_kind,
        }

    session_start = in_session[0][0]
    session_end = in_session[-1][0]
    wall_minutes = max(0, int((session_end - session_start).total_seconds() / 60))

    spawns = sum(
        1
        for _, label, _ in in_session
        if any(tok in label.upper() for tok in ("SPAWN", "AGENT"))
    )
    phases = sum(
        1
        for _, label, _ in in_session
        if any(tok in label.upper() for tok in ("PHASE", "CYCLE"))
    )

    first_idx = in_session[0][2]
    session_lines = lines[first_idx:]
    session_text = "\n".join(session_lines)

    return {
        "session_start": session_start.isoformat(),
        "session_end": session_end.isoformat(),
        "wall_time_minutes": wall_minutes,
        "agent_spawns": spawns,
        "phase_or_cycle_events": phases,
        "total_entries": len(in_session),
        "log_lines_since_session": len(session_lines),
        "log_bytes_since_session": len(session_text.encode("utf-8")),
        "boundary": boundary_kind,
    }


def workflow_artifact_stats(workflow: Path) -> dict:
    if not workflow.exists():
        return {"artifact_count": 0, "artifact_mtime_spread_hours": 0.0}
    mds = sorted(p for p in workflow.iterdir() if p.suffix == ".md" and p.is_file())
    if not mds:
        return {"artifact_count": 0, "artifact_mtime_spread_hours": 0.0}
    mtimes = sorted(p.stat().st_mtime for p in mds)
    spread_h = round((mtimes[-1] - mtimes[0]) / 3600, 2)
    return {"artifact_count": len(mds), "artifact_mtime_spread_hours": spread_h}


def context_state_hint(stats: dict) -> str:
    """Heuristic verdict on context muddiness — informational, not authoritative."""
    spawns = stats.get("agent_spawns", 0)
    phases = stats.get("phase_or_cycle_events", 0)
    bytes_ = stats.get("log_bytes_since_session", 0)
    minutes = stats.get("wall_time_minutes", 0)

    if spawns == 0 and phases == 0:
        return "fresh"
    if spawns >= 25 or bytes_ >= 80_000 or minutes >= 240:
        return "muddy"
    if spawns >= 12 or bytes_ >= 40_000 or minutes >= 120:
        return "mixed"
    return "fresh"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", action="store_true", help="emit JSON")
    ap.add_argument("--lookback-hours", type=int, default=DEFAULT_LOOKBACK_HOURS)
    args = ap.parse_args()

    if not ORCH_LOG.exists():
        out = {"error": f"orchestrator log not found at {ORCH_LOG}"}
        print(json.dumps(out, indent=2) if args.json else out["error"], file=sys.stderr)
        return 1

    stats = compute_log_stats(ORCH_LOG.read_text(), args.lookback_hours)
    stats.update(workflow_artifact_stats(WORKFLOW))
    stats["context_state_hint"] = context_state_hint(stats)

    if args.json:
        print(json.dumps(stats, indent=2))
    else:
        for k, v in stats.items():
            print(f"{k}: {v}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
