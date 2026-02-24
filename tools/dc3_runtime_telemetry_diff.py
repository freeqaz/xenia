#!/usr/bin/env python3
from __future__ import annotations

import argparse
import bisect
import json
import re
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
    unresolved_hits_by_callsite: Counter[tuple[str, str, str]] = field(default_factory=Counter)
    hot_loops: Counter[str] = field(default_factory=Counter)
    parse_errors: list[str] = field(default_factory=list)

    @property
    def summary(self) -> dict[str, Any] | None:
        return self.summaries[-1] if self.summaries else None


@dataclass
class SymbolIndex:
    starts: list[int] = field(default_factory=list)
    rows: list[tuple[int, int, str]] = field(default_factory=list)  # (start, end, name)

    def lookup(self, addr_text: str) -> str | None:
        try:
            addr = int(str(addr_text), 16)
        except ValueError:
            return None
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0 or i >= len(self.rows):
            return None
        start, end, name = self.rows[i]
        if addr < start or addr >= end:
            return None
        off = addr - start
        return f"{name}+0x{off:X}" if off else name


_SYMBOL_RE = re.compile(
    r"^(?P<name>.+?) = (?P<section>\.[^:]+):0x(?P<addr>[0-9A-Fa-f]+); .*?(?:size:0x(?P<size>[0-9A-Fa-f]+))?\b"
)


def load_symbol_index(path: Path | None) -> SymbolIndex | None:
    if path is None:
        return None
    idx = SymbolIndex()
    try:
        with path.open("r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                m = _SYMBOL_RE.match(line.strip())
                if not m:
                    continue
                name = m.group("name")
                start = int(m.group("addr"), 16)
                size = int(m.group("size"), 16) if m.group("size") else 1
                if size <= 0:
                    size = 1
                idx.starts.append(start)
                idx.rows.append((start, start + size, name))
    except FileNotFoundError:
        return None
    pairs = sorted(zip(idx.starts, idx.rows), key=lambda x: x[0])
    idx.starts = [p[0] for p in pairs]
    idx.rows = [p[1] for p in pairs]
    return idx


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
                    reason = str(e.get("reason", "<unknown>"))
                    guest_addr = _hex_key(e.get("guest_addr"))
                    callsite = _hex_key(e.get("callsite_pc"))
                    key = (reason, guest_addr)
                    out.unresolved_hits[key] += count
                    out.unresolved_hits_by_callsite[(reason, guest_addr, callsite)] += count
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


def _is_nullish_addr(addr_text: str) -> bool:
    return str(addr_text) in ("None", "0", "00000000")


def _symbolize_addr(addr_text: str, syms: SymbolIndex | None) -> str | None:
    if syms is None or _is_nullish_addr(addr_text):
        return None
    return syms.lookup(str(addr_text))


def _symbolize_addr_with_fallback(addr_text: str, syms: SymbolIndex | None) -> str:
    sym = _symbolize_addr(addr_text, syms)
    if sym:
        return sym
    return f"addr:{addr_text}"


def _dual_symbol_text(addr_text: str, orig_syms: SymbolIndex | None, decomp_syms: SymbolIndex | None) -> str:
    raw = f"addr:{addr_text}"
    if _is_nullish_addr(addr_text):
        return raw
    o = _symbolize_addr(addr_text, orig_syms)
    d = _symbolize_addr(addr_text, decomp_syms)
    if o and d:
        if o == d:
            return f"{o} [{raw}]"
        return f"orig:{o} | decomp:{d} [{raw}]"
    if d:
        return f"decomp:{d} [{raw}]"
    if o:
        return f"orig:{o} [{raw}]"
    return raw


def group_counter_keys(counter: Counter[Any], key_fn) -> Counter[Any]:
    out: Counter[Any] = Counter()
    for key, count in counter.items():
        try:
            mapped = key_fn(key)
        except Exception:
            mapped = str(key)
        out[mapped] += int(count)
    return out


def build_symbolized_unresolved_target_counter(
    tv: TelemetryView, syms: SymbolIndex | None
) -> Counter[tuple[str, str]]:
    return group_counter_keys(
        tv.unresolved_hits,
        lambda k: (k[0], _symbolize_addr_with_fallback(k[1], syms)),
    )


def build_symbolized_unresolved_caller_counter(
    tv: TelemetryView, syms: SymbolIndex | None
) -> Counter[tuple[str, str]]:
    return group_counter_keys(
        tv.unresolved_hits_by_callsite,
        lambda k: (k[0], _symbolize_addr_with_fallback(k[2], syms)),
    )


def build_symbolized_unresolved_pair_counter(
    tv: TelemetryView, syms: SymbolIndex | None
) -> Counter[tuple[str, str, str]]:
    return group_counter_keys(
        tv.unresolved_hits_by_callsite,
        lambda k: (
            k[0],
            _symbolize_addr_with_fallback(k[1], syms),
            _symbolize_addr_with_fallback(k[2], syms),
        ),
    )


def build_symbolized_hotloop_counter(tv: TelemetryView, syms: SymbolIndex | None) -> Counter[str]:
    return group_counter_keys(tv.hot_loops, lambda k: _symbolize_addr_with_fallback(str(k), syms))


def build_function_divergence_counter(tv: TelemetryView, syms: SymbolIndex | None) -> Counter[str]:
    # Aggregates two high-value signals into a function-level ranking:
    # hot-loop samples at PCs + unresolved stub hits grouped by caller function.
    out: Counter[str] = Counter()
    out.update(build_symbolized_hotloop_counter(tv, syms))
    for (_reason, caller_fn), count in build_symbolized_unresolved_caller_counter(tv, syms).items():
        out[caller_fn] += int(count)
    return out


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
    ap.add_argument("--orig-symbols", help="symbols.txt path for original build (optional)")
    ap.add_argument("--decomp-symbols", help="symbols.txt path for decomp build (optional)")
    ap.add_argument(
        "--require-summary",
        action="store_true",
        help="Exit nonzero if either file lacks a dc3_summary event",
    )
    args = ap.parse_args()

    orig = load_jsonl(Path(args.orig))
    decomp = load_jsonl(Path(args.decomp))
    orig_syms = load_symbol_index(Path(args.orig_symbols)) if args.orig_symbols else None
    decomp_syms = load_symbol_index(Path(args.decomp_symbols)) if args.decomp_symbols else None

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
        lambda k: f"{k[0]}@{k[1]} ({_dual_symbol_text(k[1], orig_syms, decomp_syms)})",
    )
    print_top_delta(
        f"Top unresolved-call stub deltas by callsite (top={args.top}):",
        top_delta(orig.unresolved_hits_by_callsite, decomp.unresolved_hits_by_callsite, args.top),
        lambda k: (
            f"{k[0]}@{k[1]} ({_dual_symbol_text(k[1], orig_syms, decomp_syms)}) "
            f"caller={k[2]} ({_dual_symbol_text(k[2], orig_syms, decomp_syms)})"
        ),
    )
    print_top_delta(
        f"Top hot-loop PC deltas (top={args.top}):",
        top_delta(orig.hot_loops, decomp.hot_loops, args.top),
        lambda k: f"{k} ({_dual_symbol_text(str(k), orig_syms, decomp_syms)})",
    )

    orig_un_target_sym = build_symbolized_unresolved_target_counter(orig, orig_syms)
    dec_un_target_sym = build_symbolized_unresolved_target_counter(decomp, decomp_syms)
    print_top_delta(
        f"Top unresolved-call stub deltas by target function (symbolized, top={args.top}):",
        top_delta(orig_un_target_sym, dec_un_target_sym, args.top),
        lambda k: f"{k[0]} target={k[1]}",
    )

    orig_un_caller_sym = build_symbolized_unresolved_caller_counter(orig, orig_syms)
    dec_un_caller_sym = build_symbolized_unresolved_caller_counter(decomp, decomp_syms)
    print_top_delta(
        f"Top unresolved-call stub deltas by caller function (symbolized, top={args.top}):",
        top_delta(orig_un_caller_sym, dec_un_caller_sym, args.top),
        lambda k: f"{k[0]} caller={k[1]}",
    )

    orig_un_pair_sym = build_symbolized_unresolved_pair_counter(orig, orig_syms)
    dec_un_pair_sym = build_symbolized_unresolved_pair_counter(decomp, decomp_syms)
    print_top_delta(
        f"Top unresolved-call stub deltas by caller->target function pair (symbolized, top={args.top}):",
        top_delta(orig_un_pair_sym, dec_un_pair_sym, args.top),
        lambda k: f"{k[0]} caller={k[2]} -> target={k[1]}",
    )

    orig_hot_sym = build_symbolized_hotloop_counter(orig, orig_syms)
    dec_hot_sym = build_symbolized_hotloop_counter(decomp, decomp_syms)
    print_top_delta(
        f"Top hot-loop deltas by function (symbolized, top={args.top}):",
        top_delta(orig_hot_sym, dec_hot_sym, args.top),
        lambda k: str(k),
    )

    orig_fn = build_function_divergence_counter(orig, orig_syms)
    dec_fn = build_function_divergence_counter(decomp, decomp_syms)
    print_top_delta(
        f"Top divergent functions (aggregate hot-loop + unresolved-caller signals, top={args.top}):",
        top_delta(orig_fn, dec_fn, args.top),
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
