# DC3/Xenia Continuation Plan (Post-Session 37)

*Last updated: 2026-02-25 (Session 37 — CRT blocker resolved)*

## Where We Are Now

### Boot progression
```
mainCRTStartup
  -> _cinit
       -> __xi loop (C initializers)     ✓ works (_ioinit injection fixed)
       -> __xc loop (C++ constructors)   ✓ FIXED (Session 37)
            -> gStringTable + gHashTable host-constructed before _cinit
            -> Symbol::PreInit override blocks original (heap not ready)
            -> all __xc entries run successfully
  -> main()                               ✓ reached
  -> game init                            ✗ BLOCKED HERE (file system assertions)
       -> File_Win.cpp:36 !strieq(drive, "game")
       -> File.cpp:768 iRoot
```

### Completed phases
- **Phase A** (baseline validation): done
- **Phase B** (String::reserve / MemOrPoolAlloc): done (root-caused to heap exhaustion)
- **Phase E** (documentation): done (STATUS.md updated through Session 37)
- **Phase F**: CRT init fixes complete (CS auto-init, _ioinit injection, Debug::Fail guard, TextStream NULL safety, PE protection, address refresh, gStringTable/gHashTable host construction)
- **Phase 1** (gStringTable fix): **COMPLETE** (Session 37)

### Current blocker
File system initialization: game code expects file paths with `"game:"` drive prefix and an initialized file root (`iRoot`). The assertions in `File_Win.cpp` and `File.cpp` indicate the Milo file system layer needs either host-side path mapping or initialization overrides.

### Critical JIT constraint discovered (Session 36)
**The x64 JIT embeds `extern_handler_` as a hardcoded immediate in generated x86 code** (`x64_emitter.cc:746`). This means:
- Guest function overrides are **permanent** once JIT-compiled callers exist
- You **cannot** override a function, then clear the override to forward to the real PPC code
- `SetupExtern(nullptr)` changes the function object but has zero effect on JIT-compiled call sites
- Any approach requiring "intercept then delegate" at the same address is fundamentally broken

This eliminates several entire categories of solutions. Valid approaches must either:
1. Write PPC code to guest memory **before** the JIT compiles it (pre-execution injection)
2. Override a function and **fully implement** it in the handler (no forwarding to same address)
3. Override a function and forward to a **different** address
4. Operate entirely on the host side (direct memory writes, no guest code involvement)

### Key discovery: /FORCE linker duplicate symbols (Session 37)
The `/FORCE` linker flag creates duplicate symbol entries. The MAP file may show a symbol at one address (e.g., gHashTable at `0x83AED0FC` in .bss) while compiled code actually references a different address (`0x83AE01C4` in .data). **Always verify global addresses via PPC disassembly when using /FORCE-linked binaries.**

---

## Working Set (Docs + Tools, Use This First)

### Backlinks (authoritative / high-signal docs)

- [STATUS.md](./STATUS.md) — latest session findings and what actually happened (current blocker source of truth)
- [DEBUGGING_TIPS.md](./DEBUGGING_TIPS.md) — crash patterns, JIT caveats, and the tooling runbook
- [GOAL.md](./GOAL.md) — long-range milestone ladder and cross-repo context (some sections are historical)
- [TODO.md](./TODO.md) — current blocker tasks only (kept intentionally short)
- [ARCHIVED.md](./ARCHIVED.md) — historical iterations, deferred backlog, and completed milestones
- [HACK_RETIREMENT_MATRIX.md](./HACK_RETIREMENT_MATRIX.md) — active Xenia workarounds and retirement triggers

### Tooling priority order (efficient daily loop)

Use the lowest-friction tool that answers the question:

1. `tools/dc3_runtime_parity_gate.sh` (with symbolization/triage) — default regression/oracle loop
2. `tools/dc3_guest_disasm.py` — fast symbolized PPC context around crash `PC/LR/CTR`
3. `tools/dc3_crash_signature_triage.py` — auto-label recurring crash classes
4. `tools/dc3_trace_on_break.sh` — repeatable headless probe around a known guest PC
5. GDB/RSP tooling (snapshot mock first, live headless RSP second) — only when script artifacts stop being enough

See also: [DEBUGGING_TIPS.md](./DEBUGGING_TIPS.md) "Tooling Runbook (Post-Debugging Leverage Loop)".

### Verified local debug tools (this workstation)

These are available and usable in the current environment:

- `powerpc-none-eabi-gdb`
- `powerpc-none-eabi-objdump`
- `powerpc-none-eabi-nm`
- `powerpc-none-eabi-readelf`
- `powerpc-none-eabi-addr2line`
- `llvm-objdump`, `llvm-readobj`
- `gdb`, `objdump`, `nm`

### Quick commands (copy/paste baseline)

**Parity gate (default loop, one-pass artifacts)**

```bash
DC3_PARITY_SYMBOLIZE=1 \
DC3_PARITY_TRIAGE=1 \
DC3_DECOMP_SYMBOL_MAP_PATH=/home/free/code/milohax/dc3-decomp/config/373307D9/symbols.txt \
~/code/milohax/xenia/tools/dc3_runtime_parity_gate.sh
```

**Fast crash disasm (log -> symbolized PPC context)**

```bash
python3 ~/code/milohax/xenia/tools/dc3_guest_disasm.py \
  --image ~/code/milohax/dc3-decomp/build/373307D9/default.xex \
  --symbols ~/code/milohax/dc3-decomp/config/373307D9/symbols.txt \
  --xenia-log ~/code/milohax/xenia/xenia-headless.log
```

**PreInit disassembly (binutils, decomp PE image)**

```bash
powerpc-none-eabi-objdump -D \
  ~/code/milohax/dc3-decomp/build/373307D9/default.exe | \
  rg -n "82556e70|<.*PreInit"
```

Notes:
- Prefer `dc3_guest_disasm.py` for XEX inputs and quick symbolized windows.
- Use `default.map` for relink-sensitive globals/CRT slots; `symbols.txt` is fine for most `.text` symbolization.

### GDB / Xenia custom hooks (RSP) — correct current usage

**Snapshot-backed (recommended first; stable)**

```bash
DC3_RSP_BRIDGE_XEX=/home/free/code/milohax/dc3-decomp/build/373307D9/default.xex \
DC3_RSP_BRIDGE_SYMBOLS=/home/free/code/milohax/dc3-decomp/config/373307D9/symbols.txt \
DC3_RSP_BRIDGE_CAPTURE_SNAPSHOT_JSON=1 \
~/code/milohax/xenia/tools/dc3_gdb_rsp_snapshot_bridge.sh
```

This wraps:
- `tools/dc3_gdb_rsp_snapshot_bridge.sh`
- `tools/dc3_gdb_rsp_mvp_mock.py`
- `tools/dc3_crash_signature_triage.py`
- `tools/dc3_guest_disasm.py`

**Live headless in-process RSP (Linux, experimental)**

```bash
~/code/milohax/xenia/build/bin/Linux/Checked/xenia-headless \
  --target=~/code/milohax/dc3-decomp/build/373307D9/default.xex \
  --store_all_context_values=true \
  --dc3_gdb_rsp_stub=true \
  --dc3_gdb_rsp_host=127.0.0.1 \
  --dc3_gdb_rsp_port=9001 \
  --dc3_gdb_rsp_break_on_connect=true
```

Then attach:

```bash
powerpc-none-eabi-gdb ~/code/milohax/dc3-decomp/build/373307D9/default.exe \
  -ex "target remote 127.0.0.1:9001"
```

Current limitation (important): some `xenia-headless` builds report no stack walker. In that mode the live stub still supports handshake/thread list/register snapshot fallback/memory reads, but live pause/step/software breakpoints are disabled.

Doc drift note: if another doc mentions `--dc3_gdb_rsp_prelaunch_sleep_ms`, treat that as stale for the current build unless the cvar is reintroduced.

---

## Phase 1: Fix gStringTable Initialization — COMPLETE (Session 37)

### Resolution
**HYPOTHESIS A confirmed**: `MemAlloc` fails because the heap isn't ready during `__xc`. `MemInit` runs from `main()`, not from `__xc`. `MemAlloc(20)` returns its size argument (`0x14`) unchanged when the heap is uninitialized.

**Fix applied**: Host-side construction (Step 5 from original plan):
- `SystemHeapAlloc` for StringTable (20 bytes) + Buf (8 bytes) + 560KB char buffer
- `SystemHeapAlloc` for gHashTable entries (92681 × 4 bytes)
- Symbol::PreInit override blocks original from calling MemAlloc
- Both globals written before `_cinit` runs

### Results
- `gStringTable` at `0x83AE01C0` = valid host-allocated pointer (`0x00037000`)
- Zero "Wasted string table" messages
- Zero `mOwnEntries` assert failures
- `_cinit` completes, `main()` reached

---

## Phase 2: Advance Past main() Entry — ACTIVE (post-Phase 1)

### Objective
Fix object factory registration and reach `App::Init()` and the main game loop.

### RESOLVED: File system (Session 37 cont.)
- gUsingCD=1 set via CheckForArchive override + direct write
- FileIsLocal assert bypass updated to current address
- gen/ symlinked for archive access
- Archive loads, config reads, MemInit runs (16MB heap allocated)

### Current blockers (Session 37 cont., post-file-system fix)
1. **Object factory instantiation failure** — `Couldn't instantiate class Mat` / `MetaMaterial` / `Cam`. sFactories appears populated but lookup fails. Same issue as Sessions 29-32.
2. **`Data is not Array` cascades** — DataNode::Array() on non-array data. From config parsing during SystemInit. Could be DTB schema mismatch or missing config sections.

### Previously expected blockers
3. **SetupFont literal corruption** — may still appear, not yet re-tested with clean init
4. **Heap exhaustion** — MemInit now runs with 16MB, but pool buckets may still exhaust

### Strategy
1. Investigate object factory registration — when do factories register, what's missing
2. Check DTB config parsing — are all expected config sections present?
3. Address SetupFont/heap issues if they appear after factory fix

### Acceptance criteria
- Game reaches `App::Run` / main game loop
- Heap allocations succeed without sanitization hacks

---

## Phase 3: Decomp Build Artifact Validation (SetupFont)

### Objective
Fix the decomp build artifact causing the bad SetupFont "font" literal reference, retire `setupfont_fix`.

### When to do this
After Phase 1 (gStringTable fix) is stable. Can be done in parallel with Phase 2 if working in dc3-decomp.

### Steps
1. Re-verify SetupFont disassembly and literals against current relink
2. Verify which `Rnd.obj` is linked and its timestamps
3. Rebuild/relink decomp and re-check `ctor1/arg2` literal at `0x82027684`
4. If fixed, disable `setupfont_fix` and run clean baseline

### Acceptance
- `setupfont_fix` no longer needed, OR decomp toolchain issue documented with exact repro

---

## Phase 4: Mat/MetaMaterial Investigation (after Phase 2)

### Objective
Classify `Couldn't instantiate class Mat` / `Unknown class MetaMaterial` as primary or secondary.

### Facts preserved from prior work
- `sFactories` map header is populated
- `RndMat::StaticClassName` and `MetaMaterial::StaticClassName` cached symbols are sane
- Relevant factory functions execute

### Steps
1. Determine if failure is: allocation failure, constructor crash, or missing dependency
2. Add targeted probe on `Hmx::Object::NewObject(Symbol)` return path
3. Correlate with allocator state

---

## Constraints and Architecture Rules

### JIT override rules (MUST follow)
| Approach | Works? | Why |
|----------|--------|-----|
| Guest override + forward to same addr | **NO** | JIT bakes handler pointer into x86 immediate; `SetupExtern(nullptr)` keeps `behavior_=kExtern` so `Call()` falls to `CallImpl()`, which also failed at runtime |
| Guest override + full host implementation | **YES** | Handler is the final implementation |
| Guest override + forward to different addr | **YES** | Different function object, different JIT code |
| PPC injection before first JIT compilation | **YES** | JIT compiles what it finds in guest memory |
| PPC injection after JIT compilation | **NO** | JIT has stale cached machine code |
| `bctrl` (indirect call) triggering override | **NO** | Only `bl` (direct call) triggers overrides |

### Relevant code paths
- `x64_emitter.cc:746` — JIT embeds `extern_handler_` as immediate
- `function.cc:63` — `SetupExtern()` always sets `behavior_=kExtern`, even when handler is null
- `function.cc:124` — `GuestFunction::Call()` checks `behavior_==kExtern && extern_handler_`; if handler is null, falls through to `CallImpl()`
- `x64_function.cc:33` — `CallImpl()` returns false if `machine_code_` is null
- `processor.cc:363` — `DemandFunction` attempts to compile PPC for extern functions (line 375), but runtime evidence showed `CallImpl()` still failed after `SetupExtern(nullptr)` — `machine_code_` state is uncertain
- `processor.cc:233` — `ApplyGuestFunctionOverride` calls `SetupExtern`

---

## Key Addresses (from 2026-02-25 MAP, post-XEX-rebuild)

**WARNING**: With `/FORCE` linker, MAP addresses for data globals may be wrong. Always verify via PPC disassembly.

### gStringTable / Symbol system
| Symbol | Address | Notes |
|--------|---------|-------|
| Symbol::PreInit | 0x82556A78 | Host override blocks original |
| Symbol::Init | 0x82557A18 | |
| Symbol::Symbol(const char*) | 0x825575A0 | Calls StringTable::Add |
| StringTable::Add | 0x82924FF0 | Host override (full implementation) |
| gStringTable | 0x83AE01C0 | .data — host-constructed before _cinit |
| gHashTable | 0x83AE01C4 | .data, right after gStringTable (MAP says 0x83AED0FC in .bss — /FORCE artifact) |

### CRT
| Symbol | Address | Notes |
|--------|---------|-------|
| _cinit | 0x8311B4B8 | Fully disassembled |
| kXcA (__xc_a) | 0x83ADED98 | C++ constructor table start |
| kXcZ (__xc_z) | 0x83ADF3B0 | C++ constructor table end |
| kXiA (__xi_a) | 0x83ADF3B4 | C initializer table start |
| kXiZ (__xi_z) | 0x83ADF3C0 | C initializer table end |
| _ioinit | 0x8361EDBC | Injected into __xi_a |

### Debug / runtime
| Symbol | Address | Notes |
|--------|---------|-------|
| Debug::Fail | 0x8354ACD0 | Has re-entrancy guard override |
| Debug::DoCrucible | 0x8354A114 | Has re-entrancy guard override |
| TextStream::op<<(const char*) | 0x829A7D38 | Has NULL safety override |
| ProtocolDebugString | 0x831F0838 | Used as PPC trampoline location |
| CODE section | 0x82320000-0x83A70000 | JIT compilation boundary |

---

## Existing CVARs (preserve semantics)

| CVAR | Default | Purpose |
|------|---------|---------|
| `--dc3_crt_skip_nui` | true | Auto-nullify NUI CRT constructors (debug bring-up default) |
| `--dc3_nui_patch_layout` | auto | NUI stub resolution mode |
| `--dc3_debug_memmgr_assert_nop_bypass` | false | Nop MemInit/MemAlloc asserts (progression aid) |
| `--dc3_debug_mempool_alloc_probe` | false | Log-only MemOrPoolAlloc probe |
| `--dc3_debug_findarray_override_mode` | off | FindArray debug modes (off/log_only/setupfont_fix/...) |
| `--dc3_debug_read_cache_stream_step_override` | false | Invasive DTB parse probe |
| `--dc3_crash_snapshot_path` | `""` | Write structured crash snapshot JSON for postmortem tools / RSP mock |
| `--dc3_gdb_rsp_stub` | false | Enable in-process headless GDB RSP listener (Linux-only) |
| `--dc3_gdb_rsp_host` | `127.0.0.1` | Listen host for in-process RSP |
| `--dc3_gdb_rsp_port` | `9001` | Listen port for in-process RSP |
| `--dc3_gdb_rsp_break_on_connect` | true | Pause on GDB client connect (when supported by build/runtime) |

---

## Session Kickoff Reference

```bash
# 1) Check state
git -C ~/code/milohax/xenia log --oneline -5
git -C ~/code/milohax/xenia status --short

# 2) Read current docs
# STATUS.md — updated through Session 36
# CONTINUATION_PLAN.md — current execution plan (this doc)
# DEBUGGING_TIPS.md — tooling runbook + crash/JIT debugging notes
# TODO.md — current blocker checklist (short form)
# GOAL.md — high-level roadmap/context
# ARCHIVED.md — historical iterations/backlog/completed milestones

# 3) Rebuild
make -C ~/code/milohax/xenia/build -f Makefile xenia-headless

# 4) Run current baseline (minimal flags for Phase 1 debugging)
timeout 20 ~/code/milohax/xenia/build/bin/Linux/Checked/xenia-headless \
  --target=~/code/milohax/dc3-decomp/build/373307D9/default.xex \
  --dc3_nui_patch_layout=auto \
  --dc3_crt_skip_nui=true \
  2>&1 | tee /tmp/dc3_baseline.log

# 5) Check for gStringTable value and crash point
grep -E "gStringTable|PreInit|StringTable|Wasted" /tmp/dc3_baseline.log

# 6) If needed: one-pass symbolized crash context
python3 ~/code/milohax/xenia/tools/dc3_guest_disasm.py \
  --image ~/code/milohax/dc3-decomp/build/373307D9/default.xex \
  --symbols ~/code/milohax/dc3-decomp/config/373307D9/symbols.txt \
  --xenia-log /tmp/dc3_baseline.log
```

---

## Immediate Next Steps (Ordered)

1. **Investigate File_Win.cpp / File.cpp assertions** — disassemble the failing code paths, read dc3-decomp source for `File_Win.cpp` and `File.cpp`, understand what `iRoot` is and how file drive letters work
2. **Implement file system path mapping or init override** — likely need to stub or override file system initialization to handle Xenia's virtual filesystem
3. **Re-check SetupFont / Mat / MetaMaterial** — these were seen in earlier sessions; check if they still appear after clean CRT progression
4. **Automate Dc3Addresses from manifest** (tech debt) — extend `generate_xenia_dc3_patch_manifest.py` to cover all hack pack addresses
