# 05 — Fork Divergence (is the fault ours or inherited?)

Branch: `headless-vulkan-linux`. See [00-source-map.md](00-source-map.md) for
where the touched subsystems live.

## Ancestry

- **Merge-base with upstream master:** `c7f61342d` (2026-01-20) — the fork is
  ~6 months behind upstream at time of writing.
- **144 fork-only commits**, of which **20 touch JIT/CPU-relevant code**
  (`src/xenia/cpu/**`, `memory.cc`, exception handlers).

## JIT/CPU-relevant fork commits

| Commit | Area | Subject |
|---|---|---|
| `6394d2a7f` | cpu/backend | Headless emulator mode + Vulkan for Linux (initial) |
| `411064457` | cpu/{backend,processor} | DC3 NUI resolver guest overrides |
| `77a2a1b13` | cpu/backend, exception_handler{,_posix}, memory | DC3 headless debugging + NUI cutover merge |
| `5eb001429` | cpu/backend | DC3 runtime parity gate + JSON telemetry |
| `610d68d78` | cpu/{backend,ppc}, exception_handler_posix, memory | Headless runtime stabilization checkpoint |
| `b1464e7d7` | cpu/backend | Instrument DC3 data-as-code crash path |
| `1e80395de` | cpu/backend | DC3 non-text call + crash register probes |
| `3e01345cd` | cpu/backend | Decomp trap-loop diagnostics/stopgaps |
| `92cc07ce1` | cpu/ppc | Linux PPC test target link deps |
| `8cb66c48c` | cpu/{backend,hir} | Fix v128 shift helper ABI in x64 JIT |
| `477cf0c56` | cpu/{backend,hir} | Fix Linux PPC JIT smoke failures |
| `dec493453` | cpu/processor, memory | DC3 tooling / hack-pack recovery checkpoint |
| `b6d1eebbd` | cpu/backend | Fix JIT crash on null trace_data |
| `3af392be6` | memory | CRT$XCU injection + MemFree heap range check |
| `31883d689` | cpu/backend | ArkFile::Read host override via PPC trampoline |
| `a486a8e66` | cpu/backend | Main-loop stall fix |
| `131e043bd` | cpu/{backend,ppc}, processor | Scale JIT prologue/epilog hook |
| `eeb3ab120` | cpu/{backend,ppc} | milo-trace X6 capture fix |
| `ef7df40da` | milo_trace.cc | milo-trace X7 tail-call oracle |
| `c3c0ec703` | cpu/ppc, cpu_flags | milo-trace X8-CE call-effects capture |

Character of these changes: mostly **instrumentation/tracing (milo-trace,
telemetry, probes — inert unless enabled)**, DC3 hack-pack plumbing, and Linux
build/signal-safety fixes. None obviously rewrites EA computation, but
`477cf0c56` + `8cb66c48c` (hir/backend semantics fixes) and `610d68d78`
(ppc + memory + exception handler) are the ones to audit first if we suspect a
fork-induced truncation bug.

## Uncommitted working-tree state (do not clobber)

- `M src/xenia/gpu/command_processor.cc` — ring-buffer assert soften (harness)
- `M src/xenia/hid/nop/nop_input_driver.{h,cc}` — 2-controller scripted input
- `?? rb3-verify/` — RB3 A/B patch harness

## Open question → resolved by [04-upstream-and-canary.md](04-upstream-and-canary.md)

DC3 boots deep into gameplay on this fork, while RB3 (both retail base and
clean TU5) faults early in DTA parsing — and DC3 also hits the same fault
family at `0x82311A94` in some paths. Whether upstream-after-2026-01-20 or
canary carries a fix for this fault family determines "port a fix" vs "write
our own".

## Verdict so far

Fault could be **either** inherited (merge-base is old) or fork-induced (20
CPU-touching commits). Discriminator experiment (cheap, definitive): build the
**merge-base commit `c7f61342d`** (or upstream master) and boot RB3 clean TU5
headless-less; if it faults the same way, the bug is inherited upstream.
Recorded as a proposed step for [06-root-cause.md](06-root-cause.md).
