# DC3/Xenia Continuation Plan (Post-Session 36)

*Last updated: 2026-02-25 (3rd-pass review)*

## Where We Are Now

### Boot progression
```
mainCRTStartup
  -> _cinit
       -> __xi loop (C initializers)     ✓ works (_ioinit injection fixed)
       -> __xc loop (C++ constructors)   ✗ BLOCKED HERE
            -> __xc[0] = PPC trampoline  ✓ executes, calls Symbol::PreInit
            -> Symbol::PreInit           ✗ produces gStringTable=0x14 (garbage)
            -> subsequent __xc entries   ✗ Symbol::Symbol -> StringTable::Add
                                            -> gStringTable=0x14 -> crash
  -> main                                  (never reached in current state)
```

### Completed phases
- **Phase A** (baseline validation): done
- **Phase B** (String::reserve / MemOrPoolAlloc): done (root-caused to heap exhaustion)
- **Phase E** (documentation): done (STATUS.md updated through Session 36)
- **Phase F partial**: CRT init fixes landed (CS auto-init, _ioinit injection, Debug::Fail guard, TextStream NULL safety, PE protection, address refresh)

### Current blocker
`gStringTable` (a `StringTable*` at `0x83AE0190`) is not properly initialized. `Symbol::PreInit` IS being called (via PPC trampoline at `__xc[0]`), but it produces `gStringTable=0x14` (garbage) instead of a valid heap pointer. Every subsequent `Symbol::Symbol` constructor passes this garbage pointer to `StringTable::Add`, which crashes or loops.

### Critical JIT constraint discovered (Session 36)
**The x64 JIT embeds `extern_handler_` as a hardcoded immediate in generated x86 code** (`x64_emitter.cc:746`). This means:
- Guest function overrides are **permanent** once JIT-compiled callers exist
- You **cannot** override a function, then clear the override to forward to the real PPC code
- `SetupExtern(nullptr)` changes the function object but has zero effect on JIT-compiled call sites
- Any approach requiring "intercept then delegate" at the same address is fundamentally broken

### Committed code state (cleanup needed before next test)
Both the PPC trampoline at `__xc[0]` (attempt #6) **and** the `StringTable::Add` guest override (attempt #7) are active simultaneously in the committed code (`dc3_hack_pack.cc`, commit `19d54a7`). Before the next real test run:
- The `StringTable::Add` override has broken forwarding code (`Processor::Execute` at same address → infinite recursion). This needs to be either removed or rewritten as a full host implementation.
- The `_cinit` diagnostic dump (60 instructions) is also committed — minor log noise, can keep for debugging or remove.

This eliminates several entire categories of solutions. Valid approaches must either:
1. Write PPC code to guest memory **before** the JIT compiles it (pre-execution injection)
2. Override a function and **fully implement** it in the handler (no forwarding to same address)
3. Override a function and forward to a **different** address
4. Operate entirely on the host side (direct memory writes, no guest code involvement)

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

## Phase 1: Fix gStringTable Initialization (BLOCKING)

### Objective
Get `Symbol::PreInit` to successfully allocate and initialize `gStringTable` so CRT `__xc` constructor iteration can complete.

### Problem tree
```
gStringTable = 0x14 (garbage, should be valid heap pointer)
  <- Symbol::PreInit(560000, 80000) called from PPC trampoline at __xc[0]
     <- PreInit allocates StringTable via new/MemAlloc
        <- HYPOTHESIS A: MemAlloc fails (heap not ready during early __xc)
        <- HYPOTHESIS B: PreInit args wrong (560000, 80000 may not match retail)
        <- HYPOTHESIS C: PreInit calls other functions that aren't ready yet
        <- HYPOTHESIS D: 0x14 is a partial/corrupted write (PreInit crashed mid-execution)
```

### Investigation steps (ordered)

**Step 1: Determine what PreInit actually does at 0x82556E70**
- **dc3-decomp source is available** (`src/system/utl/Symbol.cpp`):
  ```cpp
  void Symbol::PreInit(int stringSize, int hashSize) {
      gStringTable = new StringTable(stringSize);
      gHashTable.Resize(hashSize, nullptr);
      TheDebug.AddExitCallback(Symbol::Terminate);
  }
  ```
- PreInit allocates a `StringTable` via `operator new` (which chains to `MemAlloc`) and resizes gHashTable
- Key question: is the heap (`MemAlloc`) ready during early `__xc`? (`new StringTable` requires it)
- Verify PPC disassembly matches decomp source — use `dc3_guest_disasm.py` or `powerpc-none-eabi-objdump`
- The `0x14` garbage result could be: partial `new` return, corrupted write, or a failed allocation sentinel

**Step 2: Trace PreInit's internal calls**
- From decomp source, PreInit calls: `operator new` (→ `MemAlloc`), `StringTable::StringTable()`, `gHashTable.Resize()`, `TheDebug.AddExitCallback()`
- If these calls use `bl`, they CAN be overridden with guest function overrides (unlike `bctrl`)
- Add a temporary override on PreInit's allocator call (`operator new` / `MemAlloc`) to log args + return value
- This tells us if allocation fails, succeeds with garbage, or never happens

**Step 3: Check if the heap is initialized before __xc iteration**
- The `__xi` loop runs BEFORE `__xc`, but `__xi` only runs C initializers (e.g., `_ioinit` for I/O).
- `MemInit` is likely a C++ constructor registered via `#pragma init_seg` — meaning it's an `__xc` entry, NOT `__xi`.
- If `MemInit` is an `__xc` entry, its position relative to our trampoline at `__xc[0]` matters: if `MemInit`'s entry is AFTER `__xc[0]`, the heap is genuinely not ready when PreInit runs.
- Test: read the heap state (e.g., `MemInit` globals) from the override handler, or dump `__xc` entries to find `MemInit`'s slot.

**Step 4: Try alternative PreInit timing**
If the heap isn't ready during early `__xc`, consider:
- Move PreInit call to AFTER `__xc` completes but BEFORE `main()` (patch `_cinit` epilogue PPC)
- Or: override `Symbol::Symbol` (called via `bl`) to lazy-init on first use, implementing the override **entirely in the handler** (no forwarding — per JIT constraint)

**Step 5: Consider host-side StringTable construction**
If PreInit fundamentally can't work during `__xc`:
- Allocate guest memory from the host (`memory->SystemHeapAlloc` or similar)
- Construct a minimal valid `StringTable` object in guest memory
- Write the pointer to `0x83AE0190`
- This bypasses the guest allocator entirely

### Decision rules
- If PreInit's allocator calls succeed but the result is still garbage → investigate PreInit logic / args
- If PreInit's allocator calls fail → the heap isn't ready, need different timing or host-side construction
- If PreInit never reaches its allocator calls → it crashes before allocation, investigate early failure

### Acceptance criteria
- `gStringTable` at `0x83AE0190` contains a valid heap pointer after `__xc` iteration
- Zero "Wasted string table" messages in runtime log
- CRT `_cinit` completes and reaches `main()`

---

## Phase 2: Advance Past main() Entry (after Phase 1)

### Objective
Once CRT init completes, reach `App::Init()` and the main game loop.

### Expected blockers (from prior sessions)
These were seen in Sessions 31-32 when CRT init was bypassed differently:
1. **SetupFont literal corruption** — `SystemConfig("rnd","font")` second key is binary garbage in some decomp builds. Temporary workaround: `--dc3_debug_findarray_override_mode=setupfont_fix`
2. **Mat/MetaMaterial instantiation failure** — `Couldn't instantiate class Mat`. Factory map IS populated, cached symbols ARE sane. Root cause unclear.
3. **Heap exhaustion** — pool bucket 5 (96-byte) exhausted, `MemAlloc` returns garbage. `MemOrPoolAlloc` probe sanitizes. Root cause: heap too small or config issue.
4. **StringTable::UsedSize tight loop** — downstream of heap exhaustion, iterates unmapped/corrupted data

### Strategy
1. Run with Phase 1 fix and NO progression bypasses to establish clean baseline
2. Add bypasses incrementally to measure each blocker independently
3. Investigate heap sizing first (MemInit config values from `SystemConfig("mem")`)

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

## Key Addresses (from 2026-02-25 MAP)

### gStringTable / Symbol system
| Symbol | Address | Notes |
|--------|---------|-------|
| Symbol::PreInit | 0x82556E70 | Allocates gStringTable + gHashTable |
| Symbol::Init | 0x82557A18 | |
| Symbol::Symbol(const char*) | 0x825575A0 | Calls StringTable::Add |
| StringTable::Add | 0x82924848 | Crashes when gStringTable invalid |
| gStringTable | 0x83AE0190 | .bss, should be valid StringTable* |
| gHashTable | 0x83AED4FC | .bss (MAP symbol; note: Session 30 found active instance used by `Symbol::Symbol` at `0x83AE01CC`) |

### CRT
| Symbol | Address | Notes |
|--------|---------|-------|
| _cinit | 0x8311A6D4 | Fully disassembled Session 36 |
| kXcA (__xc_a) | 0x83ADED60 | C++ constructor table start |
| kXcZ (__xc_z) | 0x83ADF378 | C++ constructor table end |
| kXiA (__xi_a) | 0x83ADF37C | C initializer table start |
| kXiZ (__xi_z) | 0x83ADF388 | C initializer table end |
| _ioinit | 0x8361ADDC | Injected into __xi_a |

### Debug / runtime
| Symbol | Address | Notes |
|--------|---------|-------|
| Debug::Fail | 0x83547ABC | Has re-entrancy guard override |
| Debug::DoCrucible | 0x83546F00 | Has re-entrancy guard override |
| TextStream::op<<(const char*) | 0x829A7240 | Has NULL safety override |
| ProtocolDebugString | 0x831EFA44 | Used as PPC trampoline location |
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

1. **Verify `Symbol::PreInit` PPC matches decomp source** — decomp shows `new StringTable(stringSize)` + `gHashTable.Resize()` + `AddExitCallback()`. Disassemble at `0x82556E70` to confirm and identify exact `bl` targets.
2. **Determine why PreInit produces `gStringTable=0x14`** — trace `operator new` / `MemAlloc` calls (is heap ready at `__xc[0]`?)
3. **Fix gStringTable initialization** using the approach indicated by investigation (timing fix, host-side construction, or PreInit arg fix)
4. **Run clean baseline through `_cinit` completion** — verify `main()` is reached
5. **Then** return to Phase 2 (post-main progression), Phase 3 (SetupFont), Phase 4 (Mat/MetaMaterial)
