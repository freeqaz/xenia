#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import dc3_guest_disasm as guest_disasm


@dataclass
class CrashDump:
    pc: int | None = None
    lr: int | None = None
    ctr: int | None = None
    regs: dict[str, int] = field(default_factory=dict)
    fault_guest: int | None = None
    fault_host: int | None = None
    fault_access: str | None = None
    guest_code_words: dict[int, int] = field(default_factory=dict)
    stack_walk_lines: list[str] = field(default_factory=list)
    raw_lines: list[str] = field(default_factory=list)


@dataclass
class TriageFinding:
    code: str
    severity: str
    title: str
    confidence: str
    evidence: list[str] = field(default_factory=list)
    guidance: str | None = None


@dataclass
class TriageReport:
    status: str
    summary: str
    crash: CrashDump | None
    findings: list[TriageFinding]
    notes: list[str] = field(default_factory=list)

    def to_json(self) -> dict[str, Any]:
        crash = None
        if self.crash is not None:
            crash = {
                "pc": self.crash.pc,
                "lr": self.crash.lr,
                "ctr": self.crash.ctr,
                "regs": self.crash.regs,
                "fault_guest": self.crash.fault_guest,
                "fault_host": self.crash.fault_host,
                "fault_access": self.crash.fault_access,
                "guest_code_words": {f"{k:08X}": v for k, v in sorted(self.crash.guest_code_words.items())},
            }
        return {
            "status": self.status,
            "summary": self.summary,
            "crash": crash,
            "notes": self.notes,
            "findings": [
                {
                    "code": f.code,
                    "severity": f.severity,
                    "title": f.title,
                    "confidence": f.confidence,
                    "evidence": f.evidence,
                    "guidance": f.guidance,
                }
                for f in self.findings
            ],
        }


CRASH_START_RE = re.compile(r"==== CRASH DUMP ====")
PC_RE = re.compile(r"\bPC:\s*0x([0-9A-Fa-f]+)")
LR_RE = re.compile(r"\bGuest lr:\s*0x([0-9A-Fa-f]+)")
CTR_RE = re.compile(r"\bGuest ctr:\s*0x([0-9A-Fa-f]+)")
REG_RE = re.compile(r"\b(r\d+)\s*=\s*([0-9A-Fa-f]{1,16})\b")
FAULT_RE = re.compile(
    r"Fault address:\s*host=0x([0-9A-Fa-f]+)\s+guest=0x([0-9A-Fa-f]+)\s+\((READ|WRITE|EXEC|UNKNOWN)\)"
)
CODE_LINE_RE = re.compile(r"0x([0-9A-Fa-f]{8}):\s+([0-9A-Fa-f]{8})")
STACK_LINE_RE = re.compile(r"STACK WALK|lr_sp4=0x([0-9A-Fa-f]{8})|lr_bc8=0x([0-9A-Fa-f]{8})")
RESOLVE_FAIL_RE = re.compile(r"ResolveFunction\(([0-9A-Fa-f]{8})\): .*no function found", re.IGNORECASE)
TRAP_LOOP_HINT_RE = re.compile(
    r"(TrapDebugBreak|_invalid_parameter_noinfo|_vsnprintf_l|twui|debugbreak trap loop|LR=0x835B3D5C)",
    re.IGNORECASE,
)
CRT_TABLE_SUMMARY_RE = re.compile(
    r"DC3: CRT table (__x[c|i]_a\.\.__x[c|i]_z).*?:\s*(\d+) total entries, .*?(\d+) valid, .*?(\d+) nullified-oob, .*?(\d+) nullified-bisect, .*?(\d+) nullified-skip"
)


def parse_latest_crash_dump(log_path: Path) -> CrashDump | None:
    lines = log_path.read_text(encoding="utf-8", errors="ignore").splitlines()
    blocks: list[list[str]] = []
    cur: list[str] | None = None
    in_crash = False
    for line in lines:
        if CRASH_START_RE.search(line):
            if cur:
                blocks.append(cur)
            cur = [line]
            in_crash = True
            continue
        if not in_crash:
            continue
        if cur is None:
            cur = []
        cur.append(line)
        # Most crash dumps end before "Guest crashed!" or return to regular log lines.
        if "Guest crashed!" in line:
            blocks.append(cur)
            cur = None
            in_crash = False
    if cur:
        blocks.append(cur)
    if not blocks:
        return None

    block = blocks[-1]
    out = CrashDump(raw_lines=block)
    for line in block:
        if (m := PC_RE.search(line)):
            out.pc = int(m.group(1), 16)
        if (m := LR_RE.search(line)):
            out.lr = int(m.group(1), 16)
        if (m := CTR_RE.search(line)):
            out.ctr = int(m.group(1), 16)
        if (m := REG_RE.search(line)):
            out.regs[m.group(1)] = int(m.group(2), 16)
        if (m := FAULT_RE.search(line)):
            out.fault_host = int(m.group(1), 16)
            out.fault_guest = int(m.group(2), 16)
            out.fault_access = m.group(3)
        if (m := CODE_LINE_RE.search(line)):
            out.guest_code_words[int(m.group(1), 16)] = int(m.group(2), 16)
        if STACK_LINE_RE.search(line):
            out.stack_walk_lines.append(line.strip())
    return out


def parse_log_context(log_path: Path) -> dict[str, Any]:
    text = log_path.read_text(encoding="utf-8", errors="ignore")
    resolve_fail_targets = [int(m.group(1), 16) for m in RESOLVE_FAIL_RE.finditer(text)]
    trap_loop_hints = [m.group(0) for m in TRAP_LOOP_HINT_RE.finditer(text)]
    crt_tables: dict[str, dict[str, int]] = {}
    for m in CRT_TABLE_SUMMARY_RE.finditer(text):
        crt_tables[m.group(1)] = {
            "total": int(m.group(2)),
            "valid": int(m.group(3)),
            "nullified_oob": int(m.group(4)),
            "nullified_bisect": int(m.group(5)),
            "nullified_skip": int(m.group(6)),
        }
    return {
        "resolve_fail_targets": resolve_fail_targets,
        "trap_loop_hints": trap_loop_hints,
        "crt_tables": crt_tables,
    }


def _low32(v: int | None) -> int | None:
    if v is None:
        return None
    return v & 0xFFFFFFFF


def _sign_extend_low32(v: int | None) -> int | None:
    if v is None:
        return None
    x = v & 0xFFFFFFFF
    return x | (~0xFFFFFFFF) if (x & 0x80000000) else x


def classify(
    crash: CrashDump | None,
    log_ctx: dict[str, Any],
    image_reader: guest_disasm.ImageReader | None,
    symbols: guest_disasm.SymbolIndex | None,
) -> TriageReport:
    findings: list[TriageFinding] = []
    notes: list[str] = []
    if crash is None:
        if log_ctx.get("trap_loop_hints"):
            findings.append(
                TriageFinding(
                    code="trap_loop_without_crash_dump",
                    severity="warn",
                    title="Trap loop indicators found without a crash dump",
                    confidence="medium",
                    evidence=[f"matched {len(log_ctx['trap_loop_hints'])} trap-loop hints in log"],
                    guidance="Inspect log around trap markers and run trace-on-break at the reported LR/PC.",
                )
            )
            return TriageReport(
                status="warning",
                summary="No crash dump found, but trap-loop indicators were detected.",
                crash=None,
                findings=findings,
                notes=notes,
            )
        return TriageReport(
            status="ok",
            summary="No crash dump found in log.",
            crash=None,
            findings=[],
            notes=notes,
        )

    pc = crash.pc
    lr = crash.lr
    ctr = crash.ctr
    r1 = crash.regs.get("r1")
    pc_word = crash.guest_code_words.get(pc or -1) if pc is not None else None

    if image_reader is not None and pc is not None:
        try:
            pc_desc = image_reader.describe_addr(pc)
            notes.append(f"pc_section={pc_desc}")
            if not image_reader.is_executable_addr(pc):
                findings.append(
                    TriageFinding(
                        code="data_as_code_non_text_pc",
                        severity="error",
                        title="PC is outside executable guest sections",
                        confidence="high",
                        evidence=[f"PC={guest_disasm.fmt_addr(pc)} [{pc_desc}]"],
                        guidance="Trace the first control-flow transfer into non-.text memory (caller + target).",
                    )
                )
        except Exception as exc:
            notes.append(f"image_lookup_failed={exc}")

    if ctr == 0:
        evidence = [f"CTR={guest_disasm.fmt_addr(ctr)}"]
        if lr is not None:
            evidence.append(f"LR={guest_disasm.fmt_addr(lr)}")
        if any(0x40000000 <= t < 0x50000000 for t in log_ctx.get("resolve_fail_targets", [])):
            evidence.append("ResolveFunction no-function-found targets include 0x4000xxxx (HLE/XAM vtable region)")
        findings.append(
            TriageFinding(
                code="null_or_unresolved_indirect_target",
                severity="warn",
                title="Null/unresolved indirect target pattern (CTR=0)",
                confidence="medium",
                evidence=evidence,
                guidance="Check caller path for mtctr/bctrl sequence and unresolved stub telemetry/callsite counts.",
            )
        )

    if r1 is not None:
        r1_low = _low32(r1)
        if r1_low == 0:
            findings.append(
                TriageFinding(
                    code="invalid_stack_pointer_low32_zero",
                    severity="error",
                    title="Invalid guest stack pointer (r1 low 32 bits are zero)",
                    confidence="high",
                    evidence=[f"r1=0x{r1:016X}"],
                    guidance="Backtrack to the first bad prologue/epilogue or corrupted frame restore path.",
                )
            )
        if (
            crash.fault_guest is not None
            and pc_word is not None
            and (pc_word & 0xFFFF0000) == 0x94210000  # stwu r1,disp(r1)
            and r1_low is not None
        ):
            stack_delta = (crash.fault_guest - r1_low)
            if stack_delta < 0 and abs(stack_delta) <= 0x200:
                findings.append(
                    TriageFinding(
                        code="stack_underflow_on_prologue_stwu",
                        severity="error",
                        title="Function prologue stack write underflow / stack exhaustion pattern",
                        confidence="high",
                        evidence=[
                            f"PC word=0x{pc_word:08X} (stwu r1, disp(r1))",
                            f"r1(low32)=0x{r1_low:08X}",
                            f"fault_guest=0x{crash.fault_guest:08X} delta={stack_delta}",
                        ],
                        guidance="Inspect recursion/looping call path and thread stack bounds; repeated prologues may be exhausting stack.",
                    )
                )

    if crash.stack_walk_lines:
        be_count = 0
        for line in crash.stack_walk_lines:
            if "BEBEBEBE" in line.upper():
                be_count += 1
        if be_count >= 4:
            findings.append(
                TriageFinding(
                    code="stack_walk_sentinel_bebebebe",
                    severity="warn",
                    title="Stack walk shows repeated BEBEBEBE sentinels",
                    confidence="high",
                    evidence=[f"stack frames with BEBEBEBE markers: {be_count}"],
                    guidance="Treat LR/SP chain as corrupted or uninitialized; rely on first-fault PC/LR and surrounding disasm.",
                )
            )

    trap_hints = log_ctx.get("trap_loop_hints", [])
    trap_join = " ".join(trap_hints[:20])
    if trap_hints and (
        "835b3d5c" in trap_join.lower()
        or "_invalid_parameter_noinfo" in trap_join.lower()
        or "_vsnprintf_l" in trap_join.lower()
    ):
        findings.append(
            TriageFinding(
                code="crt_invalid_parameter_trap_loop",
                severity="warn",
                title="CRT invalid-parameter / trap-loop signature",
                confidence="medium",
                evidence=[f"trap-loop hints matched: {len(trap_hints)}"],
                guidance="Investigate _vsnprintf_l/_invalid_parameter path and upstream invalid state; avoid broad trap suppression first.",
            )
        )

    resolve_targets = log_ctx.get("resolve_fail_targets", [])
    if any(t == 0 for t in resolve_targets) or any(0x40000000 <= t < 0x50000000 for t in resolve_targets):
        findings.append(
            TriageFinding(
                code="resolvefunction_hle_or_null_targets",
                severity="warn",
                title="ResolveFunction failures include null/HLE-region targets",
                confidence="medium",
                evidence=[f"resolve_fail_count={len(resolve_targets)}"],
                guidance="Correlate with unresolved-call telemetry and caller symbols; distinguish expected HLE vtables from real regressions.",
            )
        )

    crt_tables = log_ctx.get("crt_tables", {})
    if crt_tables:
        for name, row in crt_tables.items():
            notes.append(
                f"{name}: valid={row['valid']} nullified_skip={row['nullified_skip']} "
                f"nullified_bisect={row['nullified_bisect']} nullified_oob={row['nullified_oob']}"
            )
        cxx = next((v for k, v in crt_tables.items() if "__xc_a" in k), None)
        if cxx and cxx.get("nullified_skip", 0) >= 50:
            findings.append(
                TriageFinding(
                    code="crt_skip_heavy_configuration",
                    severity="info",
                    title="CRT sanitizer is skipping a large constructor set",
                    confidence="high",
                    evidence=[f"C++ CRT nullified-skip={cxx['nullified_skip']} / total={cxx['total']}"],
                    guidance="If boot regresses at init, prioritize constructor export/fix work before low-impact cleanup.",
                )
            )

    if symbols and pc is not None:
        sym = symbols.lookup(pc, prefer_text=True)
        if sym:
            notes.append(f"pc_symbol={sym}")
    if symbols and lr is not None:
        sym = symbols.lookup(lr, prefer_text=True)
        if sym:
            notes.append(f"lr_symbol={sym}")

    if not findings:
        summary = "Crash parsed, but no known signature matched."
        status = "unknown"
    else:
        sev_rank = {"error": 3, "warn": 2, "info": 1}
        findings.sort(key=lambda f: (-sev_rank.get(f.severity, 0), f.code))
        top = findings[0]
        summary = f"{top.severity.upper()}: {top.title}"
        status = "triaged"

    return TriageReport(status=status, summary=summary, crash=crash, findings=findings, notes=notes)


def print_report(report: TriageReport) -> None:
    print(f"status: {report.status}")
    print(f"summary: {report.summary}")
    if report.crash:
        print(
            "crash: "
            f"PC={guest_disasm.fmt_addr(report.crash.pc)} "
            f"LR={guest_disasm.fmt_addr(report.crash.lr)} "
            f"CTR={guest_disasm.fmt_addr(report.crash.ctr)}"
        )
        if report.crash.fault_guest is not None:
            print(
                "fault: "
                f"guest={guest_disasm.fmt_addr(report.crash.fault_guest)} "
                f"host=0x{(report.crash.fault_host or 0):X} "
                f"access={report.crash.fault_access or '?'}"
            )
    if report.notes:
        print("notes:")
        for n in report.notes:
            print(f"  - {n}")
    print("findings:")
    if not report.findings:
        print("  - <none>")
        return
    for f in report.findings:
        print(f"  - [{f.severity}/{f.confidence}] {f.code}: {f.title}")
        for e in f.evidence[:6]:
            print(f"      evidence: {e}")
        if f.guidance:
            print(f"      next: {f.guidance}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Classify common DC3/Xenia crash signatures from headless logs.")
    ap.add_argument("--log", required=True, help="xenia-headless log path")
    ap.add_argument("--image", help="Guest image path (XEX/PE/raw) for section classification")
    ap.add_argument("--raw-base", help="Base guest address for raw image")
    ap.add_argument("--symbols", action="append", default=[], help="Optional symbol source(s) for note enrichment")
    ap.add_argument("--json-out", help="Write machine-readable triage report JSON")
    ap.add_argument("--allow-no-crash", action="store_true", help="Return 0 if no crash dump is present in the log")
    args = ap.parse_args()

    log_path = Path(args.log)
    if not log_path.exists():
        print(f"error: log not found: {log_path}", file=sys.stderr)
        return 2

    image_loaded = None
    image_reader = None
    raw_base = guest_disasm.parse_int(args.raw_base) if args.raw_base else None
    try:
        if args.image:
            image_loaded = guest_disasm.load_image(Path(args.image), raw_base)
            image_reader = image_loaded.reader
    except Exception as exc:
        print(f"WARN: image load failed for triage ({args.image}): {exc}", file=sys.stderr)
        image_loaded = None
        image_reader = None

    symbols = None
    if args.symbols:
        try:
            symbols = guest_disasm.load_symbols([Path(p) for p in args.symbols])
        except Exception as exc:
            print(f"WARN: symbol load failed: {exc}", file=sys.stderr)

    crash = parse_latest_crash_dump(log_path)
    log_ctx = parse_log_context(log_path)
    report = classify(crash, log_ctx, image_reader=image_reader, symbols=symbols)

    print_report(report)
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(report.to_json(), indent=2) + "\n", encoding="utf-8")
        print(f"json_report: {args.json_out}")

    if image_loaded is not None:
        image_loaded.cleanup()

    if crash is None and not args.allow_no_crash:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
