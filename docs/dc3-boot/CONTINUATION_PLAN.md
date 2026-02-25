# DC3/Xenia Continuation Plan (Post-Session 36)

*Last updated: 2026-02-25*

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

This eliminates several entire categories of solutions. Valid approaches must either:
1. Write PPC code to guest memory **before** the JIT compiles it (pre-execution injection)
2. Override a function and **fully implement** it in the handler (no forwarding to same address)
3. Override a function and forward to a **different** address
4. Operate entirely on the host side (direct memory writes, no guest code involvement)

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
- Disassemble `Symbol::PreInit` PPC code (first 30-40 instructions)
- Identify: what does it allocate? What functions does it call (via `bl`)?
- Cross-reference with dc3-decomp source if available
- Key question: does it call `MemAlloc`/`operator new` which may not be ready during early `__xc`?

**Step 2: Trace PreInit's internal calls**
- If PreInit calls allocator functions via `bl`, those CAN be overridden with guest function overrides (unlike `bctrl`)
- Add a temporary override on PreInit's allocator call to log args + return value
- This tells us if allocation fails, succeeds with garbage, or never happens

**Step 3: Check if the heap is initialized before __xc iteration**
- The `__xi` loop runs BEFORE `__xc` — does `_ioinit` or another `__xi` entry initialize the heap?
- If the heap isn't ready during `__xc[0]`, PreInit's allocations will fail
- Test: read the heap state (e.g., `MemInit` globals) from the override handler

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
| Guest override + forward to same addr | **NO** | JIT bakes handler pointer into x86 immediate |
| Guest override + full host implementation | **YES** | Handler is the final implementation |
| Guest override + forward to different addr | **YES** | Different function object, different JIT code |
| PPC injection before first JIT compilation | **YES** | JIT compiles what it finds in guest memory |
| PPC injection after JIT compilation | **NO** | JIT has stale cached machine code |
| `bctrl` (indirect call) triggering override | **NO** | Only `bl` (direct call) triggers overrides |

### Relevant code paths
- `x64_emitter.cc:746` — JIT embeds `extern_handler_` as immediate
- `function.cc:133` — `GuestFunction::Call()` checks `extern_handler_` dynamically (but JIT bypasses this)
- `processor.cc:363` — `DemandFunction` compiles PPC even for extern functions (machine_code_ set)
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
| gHashTable | 0x83AED4FC | .bss |

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
| `--dc3_crt_skip_nui` | false | Skip NUI CRT constructors (indices 98-330) |
| `--dc3_nui_patch_layout` | auto | NUI stub resolution mode |
| `--dc3_debug_memmgr_assert_nop_bypass` | false | Nop MemInit/MemAlloc asserts (progression aid) |
| `--dc3_debug_mempool_alloc_probe` | false | Log-only MemOrPoolAlloc probe |
| `--dc3_debug_findarray_override_mode` | off | FindArray debug modes (off/log_only/setupfont_fix/...) |
| `--dc3_debug_read_cache_stream_step_override` | false | Invasive DTB parse probe |

---

## Session Kickoff Reference

```bash
# 1) Check state
git -C ~/code/milohax/xenia log --oneline -5
git -C ~/code/milohax/xenia status --short

# 2) Read current docs
# STATUS.md — updated through Session 36
# MEMORY.md — updated through Session 36

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
```

---

## Immediate Next Steps (Ordered)

1. **Disassemble `Symbol::PreInit` at `0x82556E70`** to understand what it does and what it calls
2. **Determine why PreInit produces `gStringTable=0x14`** — trace its internal allocator calls
3. **Fix gStringTable initialization** using the approach indicated by investigation (timing fix, host-side construction, or PreInit arg fix)
4. **Run clean baseline through `_cinit` completion** — verify `main()` is reached
5. **Then** return to Phase 2 (post-main progression), Phase 3 (SetupFont), Phase 4 (Mat/MetaMaterial)
