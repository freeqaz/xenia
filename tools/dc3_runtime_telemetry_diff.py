#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


def _hex_key(v: Any) -> str:
    if v is None:
        return "None"
    return str(v)


@dataclass
class TelemetryView:
    path: Path
    events: list[dict[str, Any]] = field(default_factory=list)
    summaries: list[dict[str, Any]] = field(default_factory=list)
    milestones: list[str] = field(default_factory=list)
    override_registered: Counter[tuple[str, str]] = field(default_factory=Counter)
    override_hits_by_name: Counter[str] = field(default_factory=Counter)
    override_hits_by_addr: Counter[str] = field(default_factory=Counter)
    unresolved_hits: Counter[tuple[str, str]] = field(default_factory=Counter)
    hot_loops: Counter[str] = field(default_factory=Counter)
    parse_errors: list[str] = field(default_factory=list)

    @property
    def summary(self) -> dict[str, Any] | None:
        return self.summaries[-1] if self.summaries else None


def load_jsonl(path: Path) -> TelemetryView:
    out = TelemetryView(path=path)
    try:
        with path.open("r", encoding="utf-8", errors="ignore") as f:
            for lineno, line in enumerate(f, start=1):
                line = line.strip()
                if not line:
                    continue
                try:
                    e = json.loads(line)
                except json.JSONDecodeError as exc:
                    out.parse_errors.append(f"{path}:{lineno}: {exc}")
                    continue
                out.events.append(e)
                et = e.get("event")
                if et == "dc3_summary":
                    out.summaries.append(e)
                elif et == "dc3_boot_milestone":
                    name = e.get("name")
                    if isinstance(name, str):
                        out.milestones.append(name)
                elif et == "dc3_nui_override_registered":
                    name = str(e.get("name", "<unknown>"))
                    method = str(e.get("resolve_method", "<unknown>"))
                    out.override_registered[(name, method)] += 1
                elif et == "dc3_nui_override_hit":
                    count = int(e.get("count_delta", 0) or 0)
                    out.override_hits_by_name[str(e.get("name", "<unknown>"))] += count
                    out.override_hits_by_addr[_hex_key(e.get("guest_addr"))] += count
                elif et == "dc3_unresolved_call_stub_hit":
                    count = int(e.get("count_delta", 0) or 0)
                    key = (str(e.get("reason", "<unknown>")), _hex_key(e.get("guest_addr")))
                    out.unresolved_hits[key] += count
                elif et == "dc3_hot_loop_pc":
                    count = int(e.get("count", 0) or 0)
                    out.hot_loops[_hex_key(e.get("guest_pc"))] += count
    except FileNotFoundError:
        out.parse_errors.append(f"{path}: file not found")
    return out


def print_summary(label: str, tv: TelemetryView) -> None:
    print(f"[{label}] {tv.path}")
    print(f"  events={len(tv.events)} parse_errors={len(tv.parse_errors)}")
    if tv.parse_errors:
        for err in tv.parse_errors[:5]:
            print(f"  WARN {err}")
        if len(tv.parse_errors) > 5:
            print(f"  WARN ... {len(tv.parse_errors)-5} more parse errors")
    s = tv.summary
    if not s:
        print("  summary: <missing>")
        return
    rm = s.get("run_mode", {})
    nr = s.get("nui_resolver", {})
    np = s.get("nui_patch", {})
    print(
        "  summary: "
        f"reason={s.get('reason')} timeout_ms={s.get('timeout_ms')} "
        f"resolver_mode={rm.get('resolver_mode')} sig={rm.get('signature_resolver')} "
        f"overrides={rm.get('guest_overrides')}"
    )
    if nr:
        print(
            "  nui_resolver: "
            f"manifest={nr.get('manifest_hits')} symbol={nr.get('symbol_hits')} "
            f"signature={nr.get('signature_hits')} catalog={nr.get('catalog_hits')} "
            f"strict_rejects={nr.get('strict_rejects')} total={nr.get('total')}"
        )
    if np:
        print(
            "  nui_patch: "
            f"patched={np.get('patched')} overridden={np.get('overridden')} "
            f"skipped={np.get('skipped')} total={np.get('total')} layout={np.get('layout')}"
        )
    print(
        "  totals: "
        f"override_hits={s.get('total_nui_override_hits')} "
        f"unresolved_hits={s.get('total_unresolved_stub_hits')} "
        f"hot_loop_samples={s.get('total_hot_loop_samples')}"
    )


def top_delta(
    orig: Counter[Any], decomp: Counter[Any], top: int
) -> list[tuple[Any, int, int, int]]:
    rows: list[tuple[Any, int, int, int]] = []
    for key in set(orig) | set(decomp):
        o = int(orig.get(key, 0))
        d = int(decomp.get(key, 0))
        rows.append((key, d - o, d, o))
    rows.sort(key=lambda x: (-abs(x[1]), str(x[0])))
    return rows[:top]


def print_top_delta(
    title: str, rows: list[tuple[Any, int, int, int]], formatter
) -> None:
    print(title)
    if not rows:
        print("  <none>")
        return
    for key, delta, d, o in rows:
        print(f"  {formatter(key)}  delta={delta:+} decomp={d} orig={o}")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Compare DC3 runtime telemetry JSONL (original vs decomp) and rank divergences."
    )
    ap.add_argument("--orig", required=True, help="Original-run JSONL telemetry path")
    ap.add_argument("--decomp", required=True, help="Decomp-run JSONL telemetry path")
    ap.add_argument("--top", type=int, default=20, help="Top divergent entries per section")
    ap.add_argument(
        "--require-summary",
        action="store_true",
        help="Exit nonzero if either file lacks a dc3_summary event",
    )
    args = ap.parse_args()

    orig = load_jsonl(Path(args.orig))
    decomp = load_jsonl(Path(args.decomp))

    print_summary("orig", orig)
    print_summary("decomp", decomp)

    missing_summary = not orig.summary or not decomp.summary
    if args.require_summary and missing_summary:
        print("FAIL: missing dc3_summary in one or both telemetry files", file=sys.stderr)
        return 1
    if missing_summary:
        print("WARN: summary missing in one or both telemetry files (partial/crashed run?)")

    print("Milestone sequences:")
    print(f"  orig:   {orig.milestones}")
    print(f"  decomp: {decomp.milestones}")
    if orig.milestones != decomp.milestones:
        print("  WARN milestone sequences differ")

    orig_reg_names = {name for (name, _method) in orig.override_registered}
    decomp_reg_names = {name for (name, _method) in decomp.override_registered}
    only_orig = sorted(orig_reg_names - decomp_reg_names)
    only_decomp = sorted(decomp_reg_names - orig_reg_names)
    print("Override registration coverage:")
    print(f"  orig targets={len(orig_reg_names)} decomp targets={len(decomp_reg_names)}")
    if only_orig:
        print(f"  only orig ({len(only_orig)}): {', '.join(only_orig[:args.top])}")
    if only_decomp:
        print(f"  only decomp ({len(only_decomp)}): {', '.join(only_decomp[:args.top])}")
    if not only_orig and not only_decomp:
        print("  target sets match")

    print_top_delta(
        f"Top override-hit deltas by name (top={args.top}):",
        top_delta(orig.override_hits_by_name, decomp.override_hits_by_name, args.top),
        lambda k: str(k),
    )
    print_top_delta(
        f"Top unresolved-call stub deltas (top={args.top}):",
        top_delta(orig.unresolved_hits, decomp.unresolved_hits, args.top),
        lambda k: f"{k[0]}@{k[1]}",
    )
    print_top_delta(
        f"Top hot-loop PC deltas (top={args.top}):",
        top_delta(orig.hot_loops, decomp.hot_loops, args.top),
        lambda k: str(k),
    )

    if orig.summary and decomp.summary:
        nr_o = orig.summary.get("nui_resolver", {}) or {}
        nr_d = decomp.summary.get("nui_resolver", {}) or {}
        np_o = orig.summary.get("nui_patch", {}) or {}
        np_d = decomp.summary.get("nui_patch", {}) or {}
        print("Summary deltas:")
        for key in [
            ("nui_resolver.total", nr_o.get("total"), nr_d.get("total")),
            ("nui_resolver.signature_hits", nr_o.get("signature_hits"), nr_d.get("signature_hits")),
            ("nui_resolver.strict_rejects", nr_o.get("strict_rejects"), nr_d.get("strict_rejects")),
            ("nui_patch.overridden", np_o.get("overridden"), np_d.get("overridden")),
            ("total_nui_override_hits", orig.summary.get("total_nui_override_hits"), decomp.summary.get("total_nui_override_hits")),
            ("total_unresolved_stub_hits", orig.summary.get("total_unresolved_stub_hits"), decomp.summary.get("total_unresolved_stub_hits")),
            ("total_hot_loop_samples", orig.summary.get("total_hot_loop_samples"), decomp.summary.get("total_hot_loop_samples")),
        ]:
            label, o, d = key
            if isinstance(o, int) and isinstance(d, int):
                print(f"  {label}: decomp-orig={d-o:+} (decomp={d} orig={o})")
            else:
                print(f"  {label}: decomp={d} orig={o}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
