# Status: OODA Loop Iteration 5

*Last updated: 2026-02-25 (Sessions 33-36 — gStringTable NULL / "Wasted string table" infinite loop; critical JIT architecture discoveries)*

## 2026-02-25 Update (Sessions 33-36): gStringTable NULL — "Wasted string table" Infinite Loop

### Root cause
`gStringTable` is a `StringTable*` at `0x83AE0190`, initialized to NULL in `.bss`. With the zero page mapped as readable zeros in Xenia, dereferencing a NULL `gStringTable` reads `mCurBuf=0` instead of crashing. `StringTable::Add` then computes `mBuffers.size()-1` as an unsigned underflow (`0xFFFFFFFF`), entering an infinite loop that spams "Wasted string table" 700K+ times.

### Why gStringTable is NULL
`Symbol::PreInit(560000, 80000)` is the function that allocates `gStringTable`. It's normally called as a C++ `#pragma init_seg(lib)` initializer via `__xc` (the CRT C++ constructor table iterated by `_cinit`). However, the decomp build's `/FORCE` linker places `PreInit`'s init function pointer **outside** the `__xc_a...__xc_z` sentinel range (similar to the `_ioinit` problem we solved earlier). So `_cinit` never calls it.

### _cinit CRT structure (fully disassembled)
1. Calls `__savegprlr` (`0x82C1ADCC`) for register save
2. Optionally calls callback from `0x821E14A8`
3. **__xi loop**: iterates `kXiA` (`0x83ADF37C`) to `kXiZ` (`0x83ADF388`), calls non-NULL entries via `bctrl`, exits early on non-zero return
4. **__xc loop**: iterates `kXcA` (`0x83ADED60`) to `kXcZ` (`0x83ADF378`), skips entries that are `NULL (0)` or `-1 (0xFFFFFFFF)`, calls all others via `bctrl`

Key __xc loop disassembly:
```
8311A768: lis r10, 0x83AE        # kXcA high
8311A770: addi r10, r10, -0x12A0 # r10 = 0x83ADED60 = kXcA
8311A774: addi r30, r11, -0x0C88 # r30 = 0x83ADF378 = kXcZ
8311A778: or r31, r10, r10       # iterator = kXcA
8311A784: lwz r11, 0(r31)        # Load entry
8311A788: cmpwi cr6, r11, 0      # Skip if NULL
8311A790: cmpwi cr6, r11, -1     # Skip if -1
8311A798: mtctr r11              # CTR = entry
8311A79C: bctrl                  # Call entry
8311A7A0: addi r31, r31, 4       # Next
```

### Six failed approaches to inject Symbol::PreInit

1. **Guest function override at PreInit address** — `RegisterGuestFunctionOverride` only fires for direct `bl` calls. `_cinit` calls `__xc` entries via `bctrl` (indirect), so overrides DON'T fire.

2. **PPC trampoline in heap extension (`0x84F5FF00`)** — Outside XEX module range (`0x82320000-0x83A70000`). JIT doesn't know about this memory, so the trampoline code is never compiled.

3. **PPC trampoline inside HolmesClientInit body (`0x831F1680`)** — The JIT had already seen/compiled this function's original code. Overwriting mid-function after JIT compilation has no effect since the JIT doesn't re-read guest memory.

4. **Guest override at CanUseHolmes (`0x831EFB74`)** — Same `bctrl` problem as #1. Indirect calls don't trigger guest function overrides.

5. **PPC trampoline in RODATA section (`0x83F5FF00`)** — RODATA (`0x83D30000-0x83F60000`) is outside the CODE section. JIT only compiles code within CODE section boundaries.

6. **PPC trampoline in CODE section at ProtocolDebugString (`0x831EFA44`)** — This is the current approach. The trampoline is verified written correctly (readback confirmed: `__xc[0]=831EFA44`, `tramp[0]=7C0802A6`). _cinit's __xc loop starts from `kXcA`. But "Wasted string table" spam still occurs. Stack traces confirm __xc entries ARE being called (call chain: `_cinit → __xc entry → Symbol::Symbol → StringTable::Add → infinite loop`). **MYSTERY: the trampoline appears correct but gStringTable remains NULL.**

### Diagnostic evidence
- Log confirms: "Wrote Symbol::PreInit PPC trampoline (20 insns) at 831EFA44 (CODE section), injected into __xc[0] (83ADED60)"
- Verify readback: `__xc[0]=831EFA44 tramp[0]=7C0802A6 tramp[1]=90010004` ✓
- Stack frame [12] shows `LR=0x8311A7A0` which is the instruction AFTER `bctrl` in the __xc loop, confirming __xc entries execute
- 702,794 "Wasted string table" messages still appear

### Key remaining questions
1. Does the trampoline at `0x831EFA44` actually execute? (Need diagnostic inside trampoline or in Debug::Fail handler)
2. If it executes, does `Symbol::PreInit` succeed? (gStringTable may still be NULL after call)
3. Is the execution ORDER correct? (Does the trampoline run BEFORE other __xc entries that create Symbols?)
4. Does the JIT respect our written PPC code at `0x831EFA44`, or does it have a stale cached compilation from the original ProtocolDebugString function?

### Attempt 7: StringTable::Add guest override with lazy PreInit (Session 36, PARTIALLY WORKED)

Registered a guest function override on `StringTable::Add` (`0x82924848`) since it's called via `bl` (direct call from `Symbol::Symbol`), and guest overrides DO fire for `bl` calls.

**Results:**
- Override DID fire — confirmed by log: "StringTable::Add first call"
- **Zero "Wasted string table" messages** (down from 700K+) — the PPC trampoline at `__xc[0]` IS executing and setting `gStringTable=0x14` (non-zero but likely garbage/partial init)
- `this=0x14` passed to StringTable::Add (this IS gStringTable's value — meaning PreInit ran but produced garbage result `0x14`)
- `LR=0x82557600` (inside `Symbol::Symbol` — confirms call chain)

**Forwarding problem:** After calling PreInit and clearing the override, the handler tried to forward to the real StringTable::Add PPC code via `processor->Execute()`. This caused **infinite recursion** because:

### CRITICAL JIT ARCHITECTURE DISCOVERY (Session 36)

**The JIT embeds `extern_handler_` as a hardcoded x86 immediate, NOT a dynamic lookup.**

When the JIT compiles a `bl` instruction targeting a function with `behavior=kExtern`, it generates x86 code like:
```x86
mov rcx, <literal extern_handler_ pointer>  ; EMBEDDED AT JIT TIME
call guest_to_host_thunk
```

This means:
1. **Clearing `extern_handler_` at runtime has NO EFFECT on already-JIT-compiled call sites** — the old handler pointer is baked into the x86 machine code
2. **`SetupExtern(nullptr)` only affects the function object**, not the JIT-compiled callers
3. **`GuestFunction::Call()` is NOT used by JIT-compiled code** — only by `Processor::Execute()` (host-initiated calls)
4. **Once a `bl` is JIT-compiled with an extern handler, it's permanent** for all JIT-compiled callers

Source: `x64_emitter.cc` line 746:
```cpp
mov(rcx, reinterpret_cast<uint64_t>(extern_function->extern_handler()));
```

**Implication for override forwarding:** You CANNOT override a function, then clear the override to forward to the "real" PPC code via the same address. The JIT-compiled callers will forever call your handler. The only way to "forward" would be to call a DIFFERENT function address or use `Processor::Execute()` which goes through `GuestFunction::Call()` — but that also hits the handler if `extern_handler_` was embedded.

Wait — `Processor::Execute()` goes through `GuestFunction::Call()`, which DOES check `extern_handler_` dynamically. But our tests showed it still recursed. Investigation revealed `CallImpl()` (the JIT path from `GuestFunction::Call()`) returned false because `machine_code_` was null for the extern function. So `Execute` silently failed, and the PPC caller retried.

### PPC trampoline IS partially working

The test with the StringTable::Add override revealed that `gStringTable=0x14` (not NULL). This means the PPC trampoline at `__xc[0]` (`0x831EFA44`) IS executing and `Symbol::PreInit` IS being called. But PreInit produces a garbage result (`0x14`) instead of a valid StringTable pointer. This could be:
- PreInit crashing mid-execution (allocation failure?)
- PreInit's arguments wrong
- PreInit's internal calls failing silently
- `0x14` is an offset within the zero page (gStringTable read as 0x14 from partially-written memory)

### Current state of approaches
| # | Approach | Status | Failure reason |
|---|----------|--------|----------------|
| 1 | Guest override at PreInit | Failed | bctrl doesn't trigger overrides |
| 2 | PPC trampoline in heap ext | Failed | Outside XEX module range |
| 3 | PPC trampoline in HolmesClientInit | Failed | JIT stale cache |
| 4 | Guest override at CanUseHolmes | Failed | bctrl doesn't trigger |
| 5 | PPC trampoline in RODATA | Failed | Outside CODE section |
| 6 | PPC trampoline in CODE section | **Partial** | Trampoline runs, PreInit called, but gStringTable=0x14 (garbage) |
| 7 | StringTable::Add override | **Partial** | Override fires, but can't forward due to JIT embedding |

### Recommended next approaches
- **Investigate why PreInit produces gStringTable=0x14** — add host-side `Symbol::PreInit` override (it's called via `bctrl` from the trampoline, but maybe via `bl` internally?), or add PPC diagnostic instructions to the trampoline to log the return value
- **Override a function that PreInit calls via `bl`** (e.g., `MemAlloc`) — to diagnose whether allocation fails inside PreInit
- **Implement StringTable::Add entirely in the host handler** — skip forwarding, implement the string table logic in C++ on the host side
- **Patch StringTable::Add PPC prologue** — overwrite the first instructions to add a null-guard that calls PreInit, relocating the original prologue to a trampoline
- **Direct host-side gStringTable allocation** — allocate guest memory, construct a minimal StringTable object, write pointer to `0x83AE0190`

### Other fixes applied in Sessions 33-36
- **Debug::Fail re-entrancy guard**: Prevents infinite recursion when `Debug::Fail` → `DoCrucible` → `Symbol` → `StringTable::Add` → assert → `Debug::Fail`
- **TextStream::operator<<(const char*) NULL assert loop**: Guest override with vtable Print dispatch
- **PE protection extended**: Full XEX image coverage
- **All hack pack addresses refreshed** from latest MAP file

## 2026-02-25 Update (Session 32): `String::reserve` Crash Root-Caused — Heap Exhaustion via Nop'd `MemAlloc` Assert; New Blocker: `StringTable::UsedSize` Tight Loop

- Added `--dc3_debug_mempool_alloc_probe` cvar-gated probe for `MemOrPoolAlloc`:
  - overrides `MemOrPoolAlloc` (`0x834471E0`) with a host re-dispatch that logs args/return, then delegates to real guest `MemAlloc`/`PoolAlloc` via `processor->Execute()`
  - captures caller LR, requested size, file/line/name, return value
  - detailed logs on failure or from `String::reserve` caller; periodic sampling otherwise
- Root cause of `String::reserve` crash (`PC=0x82A5BC48`):
  - the crash was **not** pool free-list corruption
  - pool bucket 5 (96-byte slots, handles allocs 81-96 bytes) was genuinely **exhausted**: 241/241 nodes in use, 12 chunks, `freeList=0x00000000`
  - `FixedSizeAlloc::Refill()` → `RawAlloc()` → `_MemAllocTemp()` → `MemAlloc()` tried to allocate a new 51KB pool chunk from the main heap
  - `MemAlloc` hit its assertion at `0x83446A24` (the one nop'd by `--dc3_debug_memmgr_assert_nop_bypass`)
  - with the assert nop'd, `MemAlloc` continued past the failure with uninitialized `FreeBlockInfo` stack data, returning `0xFFFFFFFF` (stack garbage)
  - `String::reserve` treated `0xFFFFFFFF` as a valid pointer: `strmem = 0xFFFFFFFF + 4 = 0x3`, then `stw r28, -4(r29)` wrote to guest `0xFFFFFFFF` → crash
- Added sanitization in the probe: allocator returns `>= 0xF0000000` are converted to null (`0`) to prevent garbage pointer dereference
- Progression with sanitization:
  - `String::reserve` crash is eliminated
  - runtime now progresses to `Debug::Fail` "File: Mat.cpp Line: 153 Error: dir" (count=2)
  - then enters a tight SIGSEGV loop at `PC=0x82924998` (`StringTable::UsedSize`)
  - fault addresses (`0x1700FE0EA`, `0x17097FFFA`) suggest iteration over unmapped/corrupted StringTable data
  - 64K-326K SIGSEGVs accumulate during the timeout period
- Key implication: the **main heap is exhausted** (or corrupted) at the point where `Mat`/`MetaMaterial` instantiation fails
  - this likely means either `MemInit` configured the heap too small (bad/missing `SystemConfig("mem")` values), or the heap was never properly initialized due to the nop'd `MemInit` assert at `0x83447AF4`
  - next: investigate `MemInit` heap configuration and whether the `MemInit line 690` assert nop is causing underinitialized heap state

## 2026-02-25 Update (Session 31): Targeted `SetupFont` Repair Validated; `Mat/MetaMaterial` Failures Are Not a Factory-Map Symbol Corruption (New Crash: `String::reserve` / alloc fail)

- Added a targeted debug progression mode to `dc3_debug_findarray_override_mode`:
  - `setupfont_fix`
  - implementation uses the existing emulated `SystemConfig(Symbol,Symbol)` override path to repair **only** the `Rnd::SetupFont` callsite (`LR=0x8317FF14`) when arg2 is the known bad binary literal
  - repair substitutes pooled `"font"` literal `0x82017684` for the broken arg2 key
- Validation (`setupfont_fix`):
  - `SystemConfig2` probe still logs the bad `SetupFont` arg2 binary symbol and literal-sanity warning
  - targeted repair successfully returns a valid `rnd/font` config array (`-> 0x40343500` in the validation run)
  - this confirms the `SetupFont` key failure is bypassable without touching global symbol/hash state
- Factory-registration investigation (new evidence):
  - breakpoint hit matrix confirms all relevant factory path functions execute in the current run:
    - `MetaMaterial::StaticClassName`
    - `RndMat::Init`
    - `Hmx::Object::RegisterFactory`
    - `Hmx::Object::NewObject(Symbol)`
  - `Debug::Fail`-time factory dump (on `Couldn't instantiate class Mat`) shows:
    - `Hmx::Object::sFactories` map header is populated (non-empty)
    - `RndMat::StaticClassName` cached symbol slot resolves to `Mat`
    - `MetaMaterial::StaticClassName` cached symbol slot resolves to `MetaMaterial`
  - conclusion (current confidence): the `Mat/MetaMaterial` failures are **not** explained by missing registration path execution or obviously corrupted cached class symbols
- New downstream blocker (after `setupfont_fix`):
  - crash moves to `String::reserve` (`PC=0x82A5BC48`)
  - register state shows `r29=0x0000000100000003`, which matches `MemOrPoolAlloc` returning `0xFFFFFFFF` (failure sentinel), then `String::reserve` doing `ptr+4` and writing through it
  - immediate context:
    - `MemOrPoolAlloc` call at `0x82A5BBFC`
    - crash shortly after first `Couldn't instantiate class Mat`
  - this suggests a **real allocator failure path** (not just factory-symbol lookup), and likely explains the follow-on instability in error-string construction
- Additional note:
  - `ResolveFunction(0x8217AF6C)` no-op stub warnings still appear before the new crash, but current evidence points to `String::reserve` allocation failure as the direct crash cause


## 2026-02-25 Update (Session 30): `Rnd::SetupFont` `"font"` Key Failure Likely Caused by Stale `Rnd.obj` (Decomp Build Artifact), Not Hash-Table Corruption

- Added a `SystemConfig(Symbol,Symbol)` probe (enabled under `dc3_debug_findarray_override_mode=log_only`) to log both symbol arguments at the `Rnd::SetupFont` callsite (`LR=0x8317FF14`) and correlate them with `gSystemConfig` lookups.
- Corrected a probe-address pitfall:
  - the active `gHashTable` used by `Symbol::Symbol(const char*)` is the instance referenced in code (`0x83AE01CC`), not the duplicate-looking map symbol at `0x83AED0FC`
  - after correcting the probe to `0x83AE01CC`, the hash table appears healthy/populated during the bad `SetupFont` symbol event (`used ~= 6700`, valid entries pointer, `gStringTable` non-null)
- Strong new evidence from runtime + disassembly:
  - `SystemConfig2` probe at `LR=0x8317FF14` shows `s1=(rnd)` and `s2=(<bin:...>)`
  - `Rnd::SetupFont` constructs two `Symbol` temporaries and passes them to `SystemConfig` in the opposite register order:
    - ctor2 literal (`0x82053BF8`) feeds `SystemConfig` arg1 / `s1` and contains `"rnd\\0"` (this is consistent with the probe)
    - ctor1 literal (`0x82027684`) feeds `SystemConfig` arg2 / `s2` and is **non-string data** (`83 0A FA 80 ...`)
  - the pooled `"font"` string exists elsewhere in linked `.rdata` (`0x82017684`)
  - the bad `SetupFont` arg2 literal being `0x82027684` instead of a valid string pointer is the current primary issue (looks like decomp build/link artifact; the `+0x10000` delta versus `0x82017684` is suspicious)
- Build/link evidence (high confidence):
  - linker response files (`link_with_stubs.rsp`, `link_no_stubs.rsp`, `link_test.rsp`) all link `build/373307D9/obj/system/rndobj/Rnd.obj`
  - that object is older than `src/system/rndobj/Rnd.cpp` (stale object candidate)
  - conclusion: the `SetupFont` `"font"` failure is currently more likely a **stale decomp object / partial relink mismatch** than a runtime hash-table corruption bug in Xenia
- Important separation (also proven):
  - `SystemConfig("objects","Mat")` and `SystemConfig("objects","MetaMaterial")` lookups succeed in the same probe runs
  - so `Mat` / `MetaMaterial` factory-instantiation failures are likely a **separate issue** (registration/init order or another runtime problem), not just the `SetupFont` key mismatch
- Xenia-side diagnostics added:
  - `dc3_hack_pack` now logs a one-time `SetupFont` literal sanity check during the `SystemConfig2` probe path and warns if ctor1/arg2 literal is not `"font"` (current known mismatch: non-string data at `0x82027684`)
- Next debugging focus (re-prioritized):
  1. Rebuild/relink the decomp `Rnd.obj` path (or otherwise refresh `default.exe`) and re-validate `SetupFont` literal `0x82053BF8`
  2. Only continue `Symbol` hash/string-table corruption investigation if the linked `SetupFont` literals are confirmed sane
  3. Investigate `Mat` / `MetaMaterial` factory registration failures as a separate track


## 2026-02-25 Update (Session 29): `ThreadMemStack` Invalid-SP Crash Fixed (Symptom) + `SystemConfig("rnd","font")` Symbol Corruption Isolated

- Fixed the recurrent invalid guest stack-pointer crash:
  - previous failure: `PC=0x82C1BA44` (`__savegprlr_23`) with `SP=0x40`, fault write to `0xFFFFFFF0`, `LR=0x83445EE8` (`ThreadMemStack+0x8`)
  - root cause of the crash symptom was **not** `ThreadMemStack`; it was upstream `DataArray::Node` out-of-bounds returns continuing after `Debug::Fail`, which then corrupted later execution paths
- Added a targeted safety override for `merged_DataArrayNode` (`DataArray::Node` helper, `0x835421A4`):
  - normal in-range behavior is preserved
  - out-of-bounds/null-array accesses now return safe sentinel `DataNode*` values instead of invalid pointers
  - this prevents the `ThreadMemStack` invalid-SP crash while keeping the failing caller chain visible in logs
- Added caller-aware handling for `Rnd::SetupFont()` null-font path:
  - when `mFont == nullptr`, `Rnd::SetupFont` was indexing `mFont->Node(66..123)` and then cloning/assigning results
  - the safety override now detects the two `SetupFont` callsites and returns:
    - an empty-array source sentinel for the clone path (`LR=0x8317FF40`)
    - an assignment sink sentinel for the `mFont->Node(i+98)` write path (`LR=0x8318001C`)
  - this turns the `Rnd::SetupFont` null-font case into a logged progression skip instead of a crash
- Runtime outcome after the above fix:
  - `ThreadMemStack` invalid-SP crash is gone
  - runtime now progresses to a deterministic `Debug::Fail >200` spin in material/property setup (`BaseMaterial` / `Mat` path), which is a much cleaner next blocker
- Deep root-cause finding (new):
  - upgraded `FindArray` probe (`dc3_debug_findarray_override_mode=log_only`) captured a required `SystemConfig` miss with a **binary/non-printable symbol**:
    - `LR=0x83516738` (`SystemConfig(Symbol, Symbol)` second `FindArray` call)
    - `da=400E7E00`
    - `sym=<bin:83 0A FA 80 ...>`
    - `fail=1`, `found=0`
  - this is in the `Rnd::SetupFont` path and indicates the second key (`"font"`) arrives as a bogus symbol pointer rather than a valid C-string symbol
  - conclusion: the `rnd/font` failure is not just “missing config”; it is likely a **`Symbol` construction/interning corruption issue** (or related memory corruption) affecting `Symbol("font")`
- Additional corroborating symptom:
  - runtime reports `Couldn't instantiate class Mat` and `Couldn't instantiate class MetaMaterial`, followed by `BaseMaterial::PropValDifferent(...)` asserts (`base` / `node`)
  - this is consistent with the renderer/material init path operating with broken symbol/object setup after the upstream symbol corruption
- Tooling/debug improvements added during this session:
  - `FindArray` `log_only` now works as an emulated probe mode (no longer “startup note only”)
  - `FindArray` logs caller `LR`, sanitizes printable symbols, and hex-dumps binary/non-printable keys
  - “interesting” `FindArray` misses (`fail=1 && found=0`) now log even after the normal log cap
- Next debugging focus (highest value):
  - investigate why `Symbol("font")` produces a bogus pointer in `Rnd::SetupFont`
  - inspect `Symbol::Symbol(const char*)` / string-table/hash behavior and memory integrity in the period after config load / MemMgr init
  - validate whether `Mat` / `MetaMaterial` factory registration failures are downstream of the same symbol corruption or a separate init-order issue

## 2026-02-24 Update (Session 28): Manifest Duplicate-Name Stub Collision Hardening + Crash Moved Past `XTLGetLanguage`

- Found a broader patch-manifest issue while validating the `_output_l`/RCS work:
  - some manifest-resolved hack-pack stubs were mapping by duplicate symbol name to invalid/unmapped addresses (for example `XGetLocale`, `XTLGetLanguage`, `GetSystemLanguage`, `GetSystemLocale`)
  - symptom:
    - log shows a manifest remap, but the intended live function remains unstubbed
    - runtime still crashes in the real function (observed at `XTLGetLanguage`, `0x8393E9BC`)
- Hardened `PatchStub8Resolved(...)` behavior:
  - if a manifest-resolved target cannot be patched, it now logs a warning and retries the hardcoded fallback address instead of silently failing the stub application
- Refreshed locale/system-language fallback stub addresses to current map values (`build/373307D9/default.map`):
  - `XGetLocale` -> `0x8393E7B0`
  - `XTLGetLanguage` -> `0x8393E9B8`
  - `GetSystemLanguage` -> `0x83409AA8`
  - `GetSystemLocale` -> `0x83409F68`
- Validation:
  - logs now show fallback retry + successful stub application at the live addresses
  - crash moved off `XTLGetLanguage` / `XGetLocale`
  - new crash site:
    - `PC=0x82C1BA44` (`__savegprlr_23`)
    - fault write to `0xFFFFFFF0`
    - `SP=0x40`, `CTR=0`, `LR=0x83445EE8`
  - this indicates an invalid stack / bad indirect call target path (caller-side state corruption or unresolved indirect target), not the previous locale/XTL path
- Next debugging focus:
  - investigate caller at `LR=0x83445EE8` (`mtctr` / `bctrl` path leading to `CTR=0`)
  - continue using non-invasive run first; keep RCS probe for later once `ReadCacheStream` is actually reached

## 2026-02-24 Update (Session 27): `_output_l` Bridge Recovery + Pre-`ReadCacheStream` Crash (XTL path)

- Recovered and fixed the smart `_output_l` / `_woutput_l` guest formatter bridges after the partial rollback:
  - `_output_l` string-buffer paths now execute through the host bridge again (uses Xenia's `_vsnprintf_entry` shim)
  - this removed the previous long stall spinning in guest `_output_l` (`~0x8361CCF0`)
- Important relink/manifest pitfall found and fixed:
  - generic `hack_pack_stubs` manifest remaps **must not** be used for `_output_l` / `_woutput_l`
  - the manifest resolved duplicate-name entries to unrelated addresses (`_output_l: 0x841E37E0`, `_woutput_l: 0x841E9378`)
  - result: bridge registered successfully but never intercepted the live CRT formatter path
  - fix: force map-synced CRT formatter addresses (`0x8361CBE0`, `0x83622778`) and log+ignore conflicting manifest entries
- Runtime progression after the formatter fix:
  - boot now passes the prior `_output_l` stall and quickly reaches a stable crash site
  - observed crash signature (baseline and RCS-probe runs match):
    - `crash_guest=0x8393E9BC` (symbolizes near `XTLGetLanguage`)
    - fault write to `0x7014FFF8`
  - `dc3_crash_signature_triage.py` classifies this as a CRT invalid-parameter / trap-loop signature (warning-level triage)
- DTB probe validation result (important for workflow):
  - `--dc3_debug_read_cache_stream_step_override=true` correctly enables the invasive probe banner
  - however, no `DC3:RCS` read/seek hits were observed in the compared runs
  - `--break_on_instruction=0x83116664` (`ReadCacheStream`) still does not hit in the tested window
  - conclusion: current crash is occurring **before** the `ReadCacheStream` binary DTB decode path is reached (even though `_output_l` logs show DTB path-string formatting)
- Validation:
  - `xenia-headless` rebuild succeeds after bridge fix
  - `_output_l` bridge hit logs confirmed in runtime (`DC3: _output_l string ...`)
  - pre-RCS crash is reproducible in both non-invasive and invasive-RCS runs
  - invasive RCS probe remains registered but idle until the pre-RCS crash is addressed

## 2026-02-24 Update (Session 26): `ReadCacheStream` Probe Restored (Invasive BufStream Overrides)

- Restored the `--dc3_debug_read_cache_stream_step_override=true` debugging path after the partial rollback (it was previously a no-op warning in the recovered snapshot).
- Implementation detail (important):
  - this is now implemented as an **invasive BufStream probe suite** rather than a literal `ReadCacheStream` call-through override (Xenia guest extern overrides do not support calling the original guest function body).
  - active overrides:
    - `BufStream::ReadImpl` (`0x82BC3AC8`)
    - `BufStream::SeekImpl` (`0x82BC3BC8`)
  - it logs `DC3:RCS ...` step traces (read/seek counts, LR, tell/size/fail, byte previews)
- Probe caveat (same practical warning, now better explained):
  - the `BufStream::ReadImpl` host override cannot call the guest `StreamChecksumValidator::Update()` method
  - it advances `mBytesChecksummed` for visibility, but checksum validation may fail in dedicated probe runs
  - treat this as a **diagnostic-only mode** for DTB/DataArray forensics, not a correctness run
- Validation:
  - `xenia-headless` rebuild succeeded
  - default boot smoke still logs `ReadCacheStream step override disabled (default; non-invasive)`
  - probe-enabled smoke logs the invasive probe registration banner (short smokes did not yet reach DTB read path, so `DC3:RCS ReadImpl/SeekImpl` traces were not exercised in that short window)

## 2026-02-24 Update (Session 25): Partial Rollback Recovery + MemMgr/FindArray Debug Gating

- Recovered `src/xenia/dc3_hack_pack.cc` from a partial rollback while preserving newer DTB/runtime progress (`gConditional` relink fix, MemMgr assert bypass sites, `FindArray` forensics target address).
- Restored key debugging/runtime behavior that had regressed:
  - `Debug::Print` guest override (logs decomp `Debug::Print` strings to XELOG)
  - `Debug::Fail` guest override (logs LR/message and uses a layout-safe live `Debug*` probe from `r3`)
  - `_write` / `_write_nolock` guest overrides (bridge guest stdout/stderr writes to host stdout/stderr)
  - default output stub list no longer patches core CRT formatters (`_output_l`, `_woutput_l`)
- Hardened the temporary MemMgr assert bypass (`nop`) work:
  - new cvar (default off): `--dc3_debug_memmgr_assert_nop_bypass=true`
  - patches are now applied only when explicitly enabled
  - both patch sites validate the expected original instruction word before replacing with `nop` (skips with warning on layout drift / code mismatch)
  - this remains a **debug-only progression tool** and is not a correctness fix
- Hardened `DataArray::FindArray(Symbol,bool)` forensics override:
  - new mode cvar (default off): `--dc3_debug_findarray_override_mode=off|log_only|stub_on_fail|null_on_fail`
  - default `off` keeps original behavior
  - `stub_on_fail` and `null_on_fail` register the override explicitly and log returns
  - `log_only` currently leaves original behavior active (call-through logging not yet implemented with the current override API)
- Re-applied CRT slot-drift fix and removed the brittle reinjection behavior:
  - default auto-NUI skip list remains `75,98-101,210-328` (slot `69` excluded)
  - `InitMakeString` hardcoded `CRT[69]` reinjection remains removed
- Found an additional relink/staleness issue during recovery:
  - patch manifest CRT sentinels were stale (`__xc_*` / `__xi_*`) for the current decomp build
  - the CRT sanitizer now uses map-synced constructor table bounds for this build and logs when manifest sentinels differ
  - validation again shows `CRT[69] = 0x82A3F6D0 (valid)`
- Validation:
  - `make -C build xenia-headless` succeeds
  - default headless smoke reaches timeout (`RC=124`) with:
    - `ReadCacheStream` invasive step override disabled by default
    - MemMgr assert `nop` bypass disabled by default
    - `FindArray` override disabled by default
    - CRT auto-skip active without slot `69`
  - explicit debug-mode smoke confirms MemMgr `nop` bypass + `FindArray` `stub_on_fail` mode are opt-in and active only when requested

## 2026-02-24 Update (Session 24): DTB/Config Debugging Cleanup (Probe Isolation + `gConditional` Fix)

- Isolated an invasive-debugging side effect:
  - the step-by-step `ReadCacheStream` guest override (used for DTB diagnostics) performs extra `ReadImpl` + `Seek` operations and can perturb checksum/parser behavior.
  - this manifested as repeated `Unrecognized node type: %x` failures during DTB parse in probe runs.
- Made the `ReadCacheStream` step override **opt-in** (default off):
  - new cvar: `--dc3_debug_read_cache_stream_step_override=true`
  - normal runs now log that the invasive probe is disabled unless explicitly requested.
- Continued root-cause work on empty `gSystemConfig` and found another relinked global drift:
  - the `gConditional` STL list sentinel stopgap in `dc3_hack_pack.cc` was still writing an old hardcoded address (`0x82F648C4`)
  - fresh `default.map` maps old label `lbl_82F648C4` to `0x83C7D354` (`DataArray.obj`) in the current build
  - updated the stopgap to the current address
- Validation:
  - with the invasive `ReadCacheStream` override disabled (default), the DTB parse-garbage spam no longer appears
  - after the `gConditional` address refresh, `gSystemConfig` is no longer empty (observed `size=26`, non-null `nodes`)
  - remaining failures have moved downstream (`MemMgr.cpp` heap asserts), which is expected progress

### Workflow / debugging lessons reinforced

- Use non-invasive runs first; only enable heavy guest function step-overrides in dedicated probe passes.
- Treat relink-sensitive globals (not just CRT slots) as build-volatile and refresh against `build/373307D9/default.map`.

## 2026-02-24 Update (Session 23): CRT Slot Drift Root Cause (TheDebug ctor) + Fix

- Investigated the `TheDebug`/`printf` debugging path regression and found the root cause was **not** generic CRT init failure:
  - `symbols.txt` in `dc3-decomp/config/373307D9/` was stale relative to the current `default.xex`/`default.exe`, so hardcoded `TheDebug` probe addresses were invalid.
  - Current `default.map` shows `??__ETheDebug@@YAXXZ = 0x82A3F6D0`, which is **`CRT[69]`** in the current build.
  - The default auto-NUI CRT skip list still included slot `69`, and the hardcoded `InitMakeString` CRT reinjection also targeted slot `69`.
  - Result: `TheDebug` constructor was being nullified/replaced after relinks.
- Fixed in `src/xenia/dc3_hack_pack.cc` / `src/xenia/emulator.cc`:
  - removed hardcoded `CRT[69]` from the default auto-NUI skip list (`75,98-101,210-328` now)
  - retired hardcoded `InitMakeString` reinjection into `CRT[69]`
  - made the `Debug::Fail` "TheDebug" probe layout-safe by inspecting the live `Debug*` from `r3` instead of a hardcoded global address
- Validation:
  - default boot smoke (`xenia-headless` + decomp `default.xex`) now reaches timeout (`RC=124`) again under default settings with auto-NUI skip enabled
  - log confirms `CRT[69]` remains valid and is no longer reinjected
  - layout-safe probe confirms a non-null vtable on the live `Debug*` object (`TheDebug`)

## 2026-02-24 Update (Session 22): Phase 4 Headless In-Process GDB RSP MVP (Linux, Headless)

- Implemented an in-process GDB RSP listener in `src/xenia/app/emulator_headless.cc` (Linux path):
  - attached as a real `cpu::DebugListener` to the `Processor`
  - translates a minimal RSP packet subset to existing debugger APIs (`Pause`, `Continue`, `StepGuestInstruction`, `AddBreakpoint`/`RemoveBreakpoint`)
  - snapshots guest thread state via `QueryThreadDebugInfos()` for register reads
  - now uses reusable protocol/packet helpers extracted to `src/xenia/debug/dc3_gdb_rsp_protocol.h` (packet framing, checksums, hex helpers, socket packet RX/TX)
- New headless cvars (Phase 4 MVP):
  - `--dc3_gdb_rsp_stub=true`
  - `--dc3_gdb_rsp_host=127.0.0.1`
  - `--dc3_gdb_rsp_port=9001`
  - `--dc3_gdb_rsp_break_on_connect=true`
- Implemented packet coverage (MVP subset + common probes):
  - `?`, `qSupported`, `QStartNoAckMode`, `qAttached`
  - `qfThreadInfo`, `qsThreadInfo`, `qC`, `T`, `Hg`/`Hc`
  - `g`, `p`, `m`
  - `Z0` / `z0`
  - `c`, `s`, `vCont?`, `vCont;`
  - `qXfer:features:read:target.xml`
  - `qOffsets`, `qTStatus`, `qSymbol`, `QThreadSuffixSupported`, `vMustReplyEmpty`, `D`, `k`
- Build/compile validation:
  - fixed prior `emulator.cc` crash-snapshot compile issue (`GuestFunction` cast for host->guest PC mapping)
  - `xenia-headless` rebuild now succeeds on Linux (`make -C build xenia-headless`)
  - direct object compile smoke for `emulator_headless.cc` passes
  - `dc3_hack_pack.cc` rebuilt after console `printf`/`fprintf`/`vprintf` stub block hardening (manifest-aware quiet fallback + skipped accounting)

### Runtime validation status (current)

- In-process stub listener is confirmed to start (`dc3_gdb_rsp_stub: listening on 127.0.0.1:<port>` in headless log).
- Reliable live in-process attach/read smoke is now captured on Linux headless:
  - confirmed `target remote` from `powerpc-none-eabi-gdb`
  - confirmed `info registers` (LR/CTR/R1 sampled from live guest thread context in fallback mode)
  - confirmed guest memory reads (`x/4wx 0x82000000`) and `detach`
- Headless limitation (current build):
  - this `xenia-headless` build reports no stack walker, so stack-walker-dependent debugger ops are disabled in the in-process stub (live `Pause`/step/software breakpoints)
  - stub degrades gracefully instead of crashing: handshake + thread list + register snapshot fallback + memory reads still work
- Standalone protocol groundwork remains validated (mock + bridge + real GDB attach from Session 21), so packet behavior is already de-risked; remaining work is runtime integration hardening / attach timing.

## 2026-02-24 Update (Tooling): Postmortem + Parity Triage Workflow Strengthening

- Added `tools/dc3_guest_disasm.py`:
  - symbolized PPC disassembly around guest `PC/LR/CTR`
  - supports `--xenia-log` crash tuple extraction
  - supports XEX (auto-decompress), Xbox PE (`default.exe`), and raw extracted guest blobs
- Extended `tools/dc3_runtime_telemetry_diff.py`:
  - symbolized unresolved/hot-loop summaries
  - grouped symbolized rankings by unresolved caller, unresolved target, caller->target pair
  - aggregate "top divergent functions" summary (hot loops + unresolved-caller signal)
- Extended `tools/dc3_runtime_parity_gate.sh`:
  - `DC3_PARITY_SYMBOLIZE=1` emits symbolized telemetry diff and crash disasm artifacts
  - failure-path artifact generation via `EXIT` trap (no manual rerun required)
  - milestone contract verdict (`PASS/WARN/FAIL`) with policy control
  - CRT-vs-milestone triage summary to support constructor-impact prioritization
- Added `tools/dc3_crash_signature_triage.py`:
  - auto-labels common crash signatures from headless logs (invalid SP, stack-underflow prologue, non-text/data-as-code PC, trap-loop hints, etc.)
  - parity gate emits per-run triage artifacts (`orig` / `decomp`)
- Added `tools/dc3_trace_on_break.sh`:
  - headless `--break_on_instruction` wrapper with telemetry + disasm artifact capture
  - intended as the repeatable "middle ground" before interactive debugger work
- Started GDB RSP protocol groundwork with `tools/dc3_gdb_rsp_mvp_mock.py`:
  - standalone crash-snapshot-backed remote stub mock (regs + memory + break/continue packet subset)
  - useful for client/protocol experiments before in-process Xenia guest-stub integration
- Added `tools/dc3_gdb_rsp_snapshot_bridge.sh`:
  - wraps a crash log (or short headless capture run) and launches the RSP mock server
  - prints a ready-to-run GDB attach command and emits triage/disasm artifacts
  - supports snapshot-JSON-first flow (`DC3_RSP_BRIDGE_SNAPSHOT_JSON`) and optional capture request (`DC3_RSP_BRIDGE_CAPTURE_SNAPSHOT_JSON=1`)
- Added Xenia crash snapshot artifact cvar in `emulator.cc`:
  - `--dc3_crash_snapshot_path=<file.json>` writes a structured crash snapshot during guest crash dump handling

### Workflow impact

- Failing parity runs now produce investigation artifacts in one pass (telemetry diff, crash triage, crash disasm).
- Known decomp crashes can be classified quickly without manually grepping thread dumps.
- Milestone and CRT summaries now show a clearer signal for when to prioritize constructor/export work versus other cleanup.

## 2026-02-23 Update (Session 20): Stale Stopgap Addresses Fixed + Clean Boot

- Resumed from Session 19 blocker (`_vsnprintf_l` invalid-parameter trap loop at LR=0x835B3D5C).
- **ROOT CAUSE FOUND**: 4 decomp-specific stopgaps in `dc3_hack_pack.cc` had hardcoded addresses from a previous XEX build. After relinking (more units set to matching in dc3-decomp), these addresses point to unrelated live code. PatchStub8 overwrites them, corrupting the binary and causing SIGSEGV (EXIT_CODE=139).
- Verified all stopgap addresses against the current XEX binary:
  - `0x834BE094` (String::~String) — now a `bl` instruction mid-function — **STALE**
  - `0x82B324A0` (NUISPEECH::CSpCfgInst) — all zeros (target moved) — **STALE**
  - `0x83346A2C` (recursive error-report) — function epilogue — **STALE**
  - `0x834B1240` (debug/assert helper) — `bctrl` mid-function — **STALE**
  - `0x835B2D68` (_errno) — valid function prologue ✓
  - `0x835D428C` (_invalid_parameter_noinfo) — valid function prologue ✓ (new)
  - `0x83477FBC` (Hx_snprintf) — valid entry ✓
- **Disabled 4 stale stopgaps** with documentation explaining why (addresses shifted after relinking).
- **Added `_invalid_parameter_noinfo` stub** at `0x835D428C`:
  - CRT invarg.obj function checks `__pInvalidArgHandler` at `0x83C75734` — NULL since CRT not fully initialized
  - Falls through to `twui r0, 0x16` (EINVAL trap) which crashes the JIT host
  - Stubbed to `li r3, 0; blr` (just return)
- **Validation**: Game boots cleanly for 20s with NUI stubs enabled, EXIT_CODE=0:
  - Zero SIGSEGV (crash_guest seen in thread report is a handled host-level fault)
  - CRT init: 390 constructors, 264 valid, 125 skipped
  - NUI/XBC overrides: 85/85
  - 3 stopgaps active: `_errno`, `_invalid_parameter_noinfo`, `Hx_snprintf _vsnprintf_l call`
  - Thread 6 (main) alive in `D3D::FlushCachedMemory` (cache line flush loop, expected with null GPU)
  - Thread 7 spawned and running (worker thread)

### Key lesson

**Hardcoded guest addresses in stopgaps become stale when dc3-decomp relinks.** All guest address constants in `dc3_hack_pack.cc` must be verified against the current XEX/MAP after relinking. The MAP file at `build/373307D9/default.map` is currently empty (overwritten by a failed build) — once dc3-decomp builds again, regenerate it and refresh any stopgap addresses.

### Current actionable focus (Session 20)

1. **Wait for dc3-decomp build fix** (linking errors being resolved in another thread).
2. Once MAP is regenerated, refresh stale stopgap addresses (or confirm they're no longer needed).
3. Continue investigating VdSwap/Present pipeline — game renders D3D vertex buffers but never calls Present.

## 2026-02-23 Update (Session 19): `_errno` root-cause fix (partial) + `_vsnprintf_l` loop narrowing

- Re-read `STATUS.md` / `GOAL.md` and continued direct decomp boot bring-up work (no pause / no NUI detour).
- Added headless one-shot trap-loop forensics tied to `LR=0x835B3D5C`:
  - `invarg.obj` code dump (`_invalid_parameter_noinfo`, `_call_reportfault`, `_invoke_watson`)
  - `_vsnprintf_l` code dump around `0x835B3D0C` and callsite `0x835B3D58`
  - stack dump near the trap-loop frame (`SP=0x7054D8D0`)
- Confirmed `_vsnprintf_l` control-flow details:
  - `0x835B3D58` calls `_invalid_parameter_noinfo`
  - `0x835B3DF8` is a recursive/self call back to `_vsnprintf_l`
  - the trap loop is part of a recursive invalid-parameter formatting path, not a one-off trap
- Identified the helper at `0x835B2D68` as `_errno` (`dosmap.obj`) from the decomp map.
- Added a **decomp-only guest extern override** for `_errno` in `dc3_hack_pack`:
  - allocates a guest-backed `int` on `SystemHeap`
  - returns a stable guest pointer via `_errno`
  - observed registration: `_errno` -> `0x00036000`
- Validation result with `_errno` override (stable baseline):
  - decomp headless smoke returns timeout cleanly (`RC=0`)
  - NUI/XBC path still clean (`85/85` overrides, `patched=0`)
  - trap loop still occurs at `LR=0x835B3D5C`, but payload changed from bogus `0x400006A8` to valid guest errno pointer `0x00036000`
  - this proves `_errno` was one real bug in the path, but not the only cause of the loop

### Experiments attempted (not kept in stable branch state)

- Broad decomp guest override for `_invalid_parameter_noinfo` (`0x835B9B14`)
  - removed trap spam but crashed early (`RC=139`) before `dc3_hack_pack_apply_complete`
  - reverted
- Narrow `_vsnprintf_l` callsite patch (`0x835B3D58` `bl _invalid_parameter_noinfo` -> `nop`)
  - removed trap spam but also crashed early (`RC=139`)
  - reverted
- Trap-handler redirect attempts in `TrapDebugBreak`
  - useful for understanding `_vsnprintf_l` recursion/epilogue path
  - not stable, reverted

### Current actionable focus (Session 19)

1. Keep the `_errno` override (correctness + stable).
2. Find a **safe** way to break the `_vsnprintf_l` invalid-parameter recursion path without startup regressions.
3. Prefer a semantic fix near the actual invalid-parameter cause (upstream args/state) over broad CRT report suppression.

## 2026-02-23 Update (Session 18): NavList loop fix + stable trap-loop signature

- Re-read `GOAL.md` / `STATUS.md` and resumed direct decomp-boot bring-up work (not NUI work).
- Verified the prior decomp-only `except_data_82910450` stopgap at `0x82910448` was harmful:
  - it sits on a live `NavListSortMgr::ClearHeaders` tail path (`0x82910438..0x82910450`)
  - replacing it with `li r3,0; blr` created a stable loop (returns without surrounding epilogue)
  - the stopgap was removed (original bytes preserved)
- After removing the `0x82910448` stub, decomp advances to a new stable blocker:
  - repeated `tw/td` trap loop with `LR=0x835B3D5C`
  - trap log payload: `r3=0x400006A8`, `CTR=0`
- Improved trap diagnostics in `x64_emitter`:
  - trap logs now include `PC=` and `WORD=`
  - current trap path reports `PC=0` (no caller PC propagated into the helper), so `LR` remains the useful anchor
- Added/kept headless forensics useful for decomp boot triage:
  - thread-6 object/register memory dumps (`r3/r29/r30/r12`)
  - runtime one-shot patch experiments remain in headless diagnostics only
- Verified a false lead and closed it:
  - `0x400006A8` is not a DC3 XAM import ordinal on the current import list
  - it is more consistent with a status/error code payload (not `XamShowPaymentOptionsUI`)
- Stable decomp baseline after backing out unstable trap mutations:
  - `xenia-headless` exits on timeout (`RC=0`)
  - NUI/XBC path still clean (`85/85` overrides, `patched=0`)
  - thread eventually enters stable trap loop at `LR=0x835B3D5C`

### Current actionable focus (Session 18)

1. Identify the function at/around `0x835B3D0C..0x835B3D98` and why it loops on the trap path.
2. Prefer a semantic fix (status handling / helper stub) over caller-site byte patching unless the latter is proven safe.
3. Keep decomp boot tests on the stable baseline (no unstable trap-mutation stopgaps) while iterating.

## 2026-02-23 Update (Session 17): Post-NUI Cutover Boot Bring-Up (decomp-only blocker)

- Revalidated DC3 NUI/XBC resolver + guest-override path (`hybrid` default, `strict` coverage preserved).
- Added runtime parity tooling improvements:
  - `tools/dc3_runtime_parity_gate.sh` now normalizes boolean cvars (`0/1` -> `true/false`) and supports symbolized telemetry diffs.
  - `tools/dc3_runtime_telemetry_diff.py` now ranks unresolved stub hits by `(reason, target, callsite)` and can symbolize callsites/hot-loop PCs from `symbols.txt`.
- Added decomp stopgaps:
  - `except_data_82910450` patch (`0x82910448`) to stop executing exception-data blob as code.
  - `String::~String` decomp-only stopgap (`0x834BE094`) for invalid-SP prologue crash.
- Fixed `Dc3HackContext.is_decomp_layout` propagation from `emulator.cc` into `dc3_hack_pack` so decomp-only hacks actually apply.
- Current decomp 40s no-break run status:
  - NUI/XBC path resolves/overrides cleanly (`85/85`, `patched=0`).
  - unresolved-stub and hot-loop telemetry hotspots are now `0`.
  - decomp still crashes into code-like blobs with `SP=0` (data-as-code / control-flow corruption), blocking progression to `DxRnd::Present` / `D3DDevice_Swap`.
- Experimental headless-only runtime patch confirms progress:
  - one-shot patch of late-materialized helper at `0x8311B8E0` moves the fault to a later site (`__savegprlr_28`), proving the blocker is shifting and NUI/XBC is not the limiting path.
- Crash forensics update (first decomp fault, pre-runtime patch):
  - `PC=0x8311B8E4`, `LR=0x8311BAC4`, `CTR=0x00000000` (data-as-code path does not look like a normal indirect-resolve/CTR jump sequence).
  - `r1` low 32 bits are zero at first fault (`r1=0x8204FCD800000000`), confirming invalid-SP state is present at the first crash (not just later thread-status sampling).
- Added `ResolveFunction` non-text target instrumentation (x64 emitter) to capture caller->target edges for executable-but-non-`.text` calls; no hits on the current first-crash path, implying the bad control flow is bypassing the resolve thunk path.

### Current actionable focus (Session 17)

1. Track the first jump into non-text/data-as-code targets (caller + target) to identify the root corruption source.
2. Keep parity/telemetry runs on matched decomp artifacts while iterating stopgaps/instrumentation.
3. Prioritize fixes that move compilation toward `DxRnd::Present` / `D3DDevice_Swap`, not additional NUI work.

## Current Milestone: Thread 6 Alive, D3D Rendering Pipeline Active, No Frame Presentation

**Game runs full 40s, EXIT 0, 1 SIGSEGV (handled), Thread 6 executing Game::PostUpdate → D3D rendering.**

Thread 6 is no longer frozen. It cycles through D3D functions at ~247 CS/sec:
- `D3D::FlushCachedMemory` (memory.obj) — now a no-op thanks to dcbf fix
- `D3D::UnlockResource` (resource.obj) — unlocking vertex/index buffers
- `D3DVertexBuffer_Unlock` (resource.obj) — vertex buffer management
- `PIXAddPixOpWithData` (pix.obj) — performance instrumentation markers

**Key observation**: `VdSwap` is imported but **never called** — the game prepares render data
(vertex buffers, PIX markers, cache flushes) but never submits a frame for display. The null GPU
backend does NOT block (IssueSwap is a synchronous no-op), so the blocker is upstream: the game's
rendering pipeline never reaches the Present/Swap call.

### Session 16 Fixes

1. **dcbf/dcbst → no-op in JIT** (x64_seq_memory.cc): Xenia JIT compiled PPC `dcbf` (data cache
   block flush) to x86 `clflush`, which faults on unmapped physical memory. Cache line flushing is
   unnecessary in an emulator. This eliminated 1.4M signal handler round-trips per run.

2. **clflush/prefetch fault handler** (mmio_handler.cc): Safety net — detects x86 `clflush`
   (0F AE /7) and `prefetch` (0F 18 /0-3) instructions in the soft fault handler. When they fault
   on unmapped memory, skips them instead of crashing. Handles REX prefix + ModR/M + SIB + disp.

### Thread State (test74)
| Thread | State | LR | Notes |
|--------|-------|-----|-------|
| 1-5 | Idle | 0x00000000 | Not yet started |
| 6 | **ALIVE** | Cycling | Game::PostUpdate → D3D rendering |
| 7 | Sleeping | 0x8302D1BC | RtlSleep (joypad polling) |

### Metrics
| Metric | Value |
|--------|-------|
| SIGSEGV total | 1 (handled) |
| CS counts | 39781→48654 over 39s (+247/sec, steady growth) |
| VdSwap calls | 0 |
| ResolveFunction failures | 9 (2 kernel imports: 0x40000058 ×5, 0x400042E0 ×4) |
| Unimplemented instructions | 0 |

## OBSERVE — Current Blocker Analysis

### VdSwap Never Called
The game is executing D3D vertex buffer operations in a loop but never calls VdSwap (Present).
This means the rendering pipeline never completes a frame. Possible causes:

1. **D3D device not fully initialized** — D3DDevice_Create or setup function returned error/stub value
2. **Missing D3D state setup** — render target, depth stencil, or viewport not configured
3. **Game loop stuck in resource loading** — preparing buffers but waiting for something to complete
4. **Present is called but through a stub** — the function is implemented in an XDK .obj but
   depends on something that's broken

### Threads 1-5 Idle
Five threads are created but never start executing (LR=0, SP at initial stack top). These are
likely worker threads that get signaled after initialization completes. If the main thread (6) is
stuck in the render pipeline, these threads may never activate.

### What Functions Thread 6 Executes
JIT IP samples across 13 reports show Thread 6 cycles through:
- `Game::PostUpdate` (Game.obj) — main game tick
- `NgEnviron::Select` / `UpdateApproxLighting` (Env_NG.obj) — environment rendering
- `D3D::UnlockResource` (resource.obj) — D3D resource management
- `D3DVertexBuffer_Unlock` (resource.obj) — vertex buffer unlock
- `PIXAddPixOpWithData` (pix.obj) — profiling markers
- `__savegprlr` / `__restgprlr` — register save/restore thunks
- `guest 0x00000000` — null function calls (stubs)

This is REAL game rendering code, not initialization. The game has reached the render loop
but can't complete a frame.

## ORIENT — What Does It Mean?

### Critical Path (updated)
```
XEX loads → imports resolve → NUI stubbed → CRT init → main() → App::Init →
Game::PostUpdate → D3D rendering → [STUCK: never reaches Present/Swap] → frame displayed
```

We are past main(), past App::Init(), and into the game's main render loop. The game is
updating animations, processing environment lighting, and managing vertex buffers. But it
never submits a completed frame.

### Hypothesis: D3D Present path broken or unreachable
The game calls `Game::PostUpdate` which drives rendering through NgEnviron. The D3D
vertex buffer lock/unlock cycle suggests it's preparing geometry. But without calling
Present/Swap, the frame is never submitted. Either:
- A D3D function in the present path is stubbed/missing and returns error
- The rendering state machine requires a condition that's never met
- A synchronization primitive (event/semaphore) blocks the present call

## DECIDE — Highest-Leverage Fixes

**Priority 1**: Trace why VdSwap is never called. Find D3DDevice_Present or the
swap chain in the MAP, check if it's called, and identify what blocks it.

**Priority 2**: Check what `guest 0x00000000` calls represent — these are null
function pointer calls happening frequently. If a critical D3D function resolves
to null, that could be why the pipeline breaks.

**Priority 3**: Look for D3D initialization functions that might have returned
error values (stubs returning 0 or -1 instead of success).

## ACT — See TODO.md

---

## 2026-02-23 Update (Phase 2 Consolidation): Non-NUI DC3 Hack-Pack Extraction (Stable, Partial)

- Added `src/xenia/dc3_hack_pack.h` / `src/xenia/dc3_hack_pack.cc`
- Extracted non-NUI DC3 workarounds from `src/xenia/emulator.cc` into the hack-pack module:
  - Xapi thread notify stub
  - decomp runtime stopgaps (stack bump, RODATA writable range, zero-page + Linux null-guard page)
  - debug/runtime stubs (`_output_l`, Holmes, `Debug::Fail`, locale/debug helpers, `String::operator+=`, etc.)
  - unresolved import thunk / XEX marker cleanup
  - CRT table sanitizer + `InitMakeString` injection
  - import-indirection diagnostics
- `emulator.cc` now calls `ApplyDc3HackPack(...)` for this non-NUI block and logs per-category summary counts.

### Important Phase 2 Validation Notes (semantic-drift bugs caught/fixed)

During extraction validation, two accidental behavior changes caused real regressions and were removed:

1. **Unsafe writable-range widening** (reverted)
   - A drifted hack-pack version widened the decomp writable-image workaround from:
     - `0x82000000-0x822E0000` (RODATA only)
     to
     - full image range (`0x82000000-0x83ED0000`)
   - This caused original-build crashes and `BaseHeap::Protect` region-span failures.
   - Final behavior is restored to **RODATA-only**, matching the original inline logic.

2. **Extra `.text` zero-word mass patching** (removed)
   - A drifted hack-pack version included a decomp-only experiment that replaced zero words in `.text` with `blr`.
   - This was **not part of the original inline path** and caused original-build SIGSEGV during hack-pack apply.
   - The block was removed to preserve no-behavior-change extraction semantics.

### Deferred (explicit)

- `fake_kinect_data` / skeleton injection remains inline in `src/xenia/emulator.cc`
  - An extraction attempt was started but deferred to keep Phase 2 stable.
  - The failure was traced to unrelated semantic drifts in the extracted hack-pack path, not the skeleton logic itself.
  - Reattempt with exact-semantic extraction (or host-side hook replacement) is tracked in the retirement matrix.

### Validation (2026-02-23)

- `./xb build --config=debug --target=xenia-headless --target=xenia-core-tests --no_premake -j 8` ✅
- `xenia-core-tests "[dc3_nui_patch_resolver]"` ✅ (`648 assertions / 10 test cases`)
- `tools/dc3_nui_cutover_gate.sh` ✅ (`default` + `strict`, original + decomp)
- `tools/dc3_runtime_parity_gate.sh` ✅ (`hybrid`)
- `DC3_PARITY_MODE=strict tools/dc3_runtime_parity_gate.sh` ✅ (`strict`)

## 2026-02-23 Update (Phase 4 Manifest Contract Hardening): Schema + Safe Fallbacks

- `dc3-decomp` manifest emitter (`generate_xenia_dc3_patch_manifest.py`) now writes:
  - `schema_version`
  - `build_identity` block (`title_id`, `build_label`)
  - existing fields retained for compatibility (`format_version`, top-level `title_id`, `build_label`)
- Xenia manifest parser (`Dc3LoadNuiPatchManifest`) now:
  - requires `schema == "xenia.dc3.nui_patch_manifest"`
  - requires supported schema/version (`schema_version` or `format_version`, currently `1`)
  - rejects manifests with no `targets` table
  - logs actionable warnings and safely falls back to symbol/signature/catalog inputs
- Resolver unit tests updated for the stricter manifest parser requirements (`schema` + `schema_version`)

### Additional safety hardening (important workflow fix)

- Xenia now disables **manifest target address resolution** (but not the whole resolver pipeline) when the loaded XEX `.text` fingerprint does not match the manifest fingerprint.
  - This prevents stale/wrong manifests from overriding function addresses and crashing hybrid runs.
  - Resolver falls back to symbol/signature/catalog sources automatically.

### Known remaining manifest hazard (tracked)

- A manifest can still be internally inconsistent if it is regenerated from a mismatched artifact pair (for example: stale `.map` with a different `.xex`) but the `.xex` fingerprint matches.
  - This is a build-provenance problem, not a parser crash problem.
  - Follow-up work: add stronger manifest/source pairing checks or avoid prioritizing `.map` addresses when pair integrity is unknown.

## 2026-02-23 Update (Phase 1 Consolidation): Runtime Parity Gate + JSONL Telemetry

- Implemented DC3 structured runtime telemetry (cvar-gated JSONL):
  - `--dc3_runtime_telemetry_enable=true`
  - `--dc3_runtime_telemetry_path=<file.jsonl>`
- Telemetry currently emits:
  - `dc3_boot_milestone`
  - `dc3_nui_override_registered`
  - aggregated `dc3_nui_override_hit`
  - aggregated `dc3_unresolved_call_stub_hit`
  - aggregated `dc3_hot_loop_pc`
  - `dc3_summary`
- Telemetry hooks are wired into:
  - DC3 NUI/XBC resolver/apply path (`emulator.cc`)
  - guest extern call path + unresolved no-op stub path (`x64_emitter.cc`)
  - headless timeout / normal thread finish (`emulator_headless.cc`)
- Implemented parity gate: `tools/dc3_runtime_parity_gate.sh`
  - Runs original + decomp with telemetry enabled
  - Supports `DC3_PARITY_MODE=hybrid|strict`
  - Hard-fails on NUI/XBC resolver/apply regressions
  - Warn-only on milestone/hot-loop/unresolved diffs (initially)
- Validation (2026-02-23):
  - Existing `tools/dc3_nui_cutover_gate.sh` still passes (`default` + `strict`)
  - `xenia-core-tests "[dc3_nui_patch_resolver]"` still passes (`648 assertions / 10 test cases`)
  - `tools/dc3_runtime_parity_gate.sh` passes in:
    - `hybrid`
    - `strict`
  - Initial parity telemetry signal identifies a decomp-only hot loop PC (`8291044C`) as warn-only data

## 2026-02-23 Update (Sessions 12-13): JIT Indirect Call Fix + Game Stability

### Major Breakthrough: Game Runs Without Crashing

The game now runs for the full 30-second timeout (exit code 124) with **zero SIGSEGV crashes**.
4.4MB of log output. The critical path advanced from "CRT init crash" to "game main loop running
but stuck in NUI callback dispatch."

### Fixes Applied (x64_emitter.cc, x64_code_cache.cc, x64_code_cache.h)

1. **CallIndirect bounds check**: JIT indirect calls (`bctrl`) now check if the guest address
   is within the indirection table range `[0x80000000, 0x9FFFFFFF]`. Out-of-range addresses
   (like XAM COM vtable entries at `0x40002830`) fall through to the resolve function thunk
   instead of reading unmapped memory.

2. **Resolve function thunk for out-of-range calls**: The slow path uses the resolve function
   thunk (which properly saves/restores volatile JIT registers) instead of calling ResolveFunction
   as a C function. This prevents clobbering rsi (context), rdi (membase), r10, r11, xmm4-15.

3. **No-op return stub for unresolvable functions**: When `ResolveFunction` can't find a function
   (null target, XAM COM entries, garbage pointers), it returns a single `ret` instruction placed
   in the code cache. This lets the game continue instead of crashing.

4. **Indirection table bounds checks**: `AddIndirection` and `PlaceGuestCode` in x64_code_cache.cc
   now skip indirection table updates for guest addresses outside the valid range.

5. **Register ordering fix (Linux SysV ABI)**: Fixed the old-style resolve path to read `rsi`
   (context) into `rdi` BEFORE overwriting `esi` with the target address.

6. **Made indirection table constants public**: `kIndirectionTableBase` and `kIndirectionTableSize`
   moved from `protected` to `public` in x64_code_cache.h for use in bounds checks.

### Current Blocker: NUI Callback Dispatch Loop

The game is stuck in a tight loop at guest `0x834B1F40` iterating a linked list of callback
function pointers. The function pointers are garbage:
- `0x38600000` (PPC instruction `li r3, 0` — not a valid function address)
- `0x00000000` (null)

These are handled gracefully by the no-op stub (~29,754 calls each in 30 seconds), but the loop
never terminates. The callback list at `[r30+0x20]` (object at `0x83C16B40`) is part of the NUI
subsystem and was never properly initialized because NUI was stubbed.

```
0x834B1F40: lwz r11, 0x0008(r29)   ; load function pointer from list node
0x834B1F44: mtctr r11               ; CTR = function pointer
0x834B1F48: bctrl                   ; call → no-op stub returns
0x834B1F4C: lwz r29, 0x0000(r29)   ; r29 = next node in linked list
0x834B1F50: cmplw cr6, r29, r30    ; compare with end sentinel
0x834B1F54: bne- cr6, 0x834B1F40   ; loop if not done → infinite
```

### Documentation Created

- `docs/dc3-boot/DEBUGGING_TIPS.md` — comprehensive debugging guide covering JIT architecture,
  memory map, indirection table, volatile register dangers, crash patterns, and diagnostic
  instrumentation. Captures hard-won knowledge from sessions 10-13.

## 2026-02-23 Update (Guest Override / Patch Hardening In Progress)

- Added CPU/backend support for **guest function extern overrides on normal direct guest calls** (`bl` path), not just import-thunk externs.
- Added `Processor` guest override registry (`Register/ClearGuestFunctionOverride*`) and extern binding during guest function lookup.
- DC3 NUI/XBC stub path can now use **guest extern overrides** for eligible simple `li r3, {0|-1}; blr` stubs (cvar-gated), with byte-patch fallback for unsupported/unresolved entries.
- Preserved fake Kinect skeleton injection behavior by not overriding `NuiSkeletonGetNextFrame` when `fake_kinect_data` is enabled.
- Added safer byte-patch behavior:
  - skips zero-filled targets
  - skips unmapped/translate-failed targets
  - validates target is inside `.text` when section metadata is available
- Added `.text` FNV-1a fingerprint logging and `dc3_nui_patch_layout=auto|original|decomp` with optional fingerprint-based auto matching cvars (`dc3_nui_layout_fingerprint_{original,decomp}`).
- Added optional fingerprint cache file support (`docs/dc3-boot/dc3_nui_fingerprints.txt`) with auto-probe in Xenia.
- Added a first-pass **resolver pipeline** for DC3 NUI/XBC patch targets:
  - patch manifest JSON (`xenia_dc3_patch_manifest.json`) -> symbol manifest (`symbols.txt`-style `.text` entries) -> signature hook (stub) -> catalog fallback
  - `dc3_nui_patch_resolver_mode=hybrid|strict` (legacy mode removed later in the cutover)
  - per-entry resolver-method logging in patch/override output
- Added `dc3-decomp` post-link manifest emitter (`scripts/build/generate_xenia_dc3_patch_manifest.py`) and `build_xex.py` integration (best-effort generation).
- Fingerprint note: the decomp `.text` fingerprint must be computed from the **final packed XEX** (after thunk-marker rewriting), not the pre-pack `default.exe`.
- Added override cleanup on title terminate / new launch path to avoid stale mappings across runs.

This is an **implementation enabler** for retiring raw guest byte patches. Full semantic resolver work (fingerprint cache + symbol/signature resolution) is still in progress.

## Current Milestone: Game Runs Full 40s, CRT + Init Complete, Memory Scanning Loop

Progress: **Game runs stably for full 40-second timeout with EXIT 0, only 2 SIGSEGV (handled).**
All JIT compilation crashes fixed. SIGBUS handler added. Soft fault handler for unmapped guest
memory. Pre-mapped guard pages. Thread 6 (main game) reaches initialization scanning phase at
0x834AA920 — walking through all of guest memory (SP traverses 0x00000000-0xFFFFFFFF range).
Thread 7 (joypad) running steadily. CS operations balanced and growing (~735/3s interval).
No GPU commands issued yet — game hasn't reached rendering. 13 thread status reports per run.

## OBSERVE — What happens when we run the test?

### What works
- XEX loads, 323 thunks + 501 variable imports resolved (100%)
- 62/62 NUI (Kinect SDK) functions stubbed in guest memory
- XapiCallThreadNotifyRoutines stubbed (CRT list corruption bypass)
- Main thread stack increased 256KB -> 1MB
- RODATA pages (0x82000000-0x82410000) made writable for /FORCE unresolved globals
- CRT sanitizer: **364/390 constructors execute** (26 nullified -- all point to 0x82000000)
- Main XThread starts, VdSwap imported
- RtlEnterCS=186, RtlInitCS=10, RtlLeaveCS=185 before crash
- MAP address extractor validated: 70/70 symbols match emulator.cc (Agent E)

### Where it crashes
- **RtlpCoalesceFreeBlocks** (rtlheap.obj @ 0x830DC644) -- SIGSEGV reading guest address 0
- Heap metadata overwritten: r30=r31=0x536F7264, r25=0xFEEEFEEE
- Same pattern seen in Session 5 -- persistent, deterministic decomp-side bug
- Crash occurs during CRT static initialization (before game `main()`)

### Wave 1 key finding: "Sord" is NOT a string literal (Agent B)
- Zero matches for "Sord" in any .obj file or MAP symbol
- 0x536F7264 is **corrupted heap free-list pointer** data, not string data
- The ASCII interpretation as "Sord" is coincidental
- 0xFEEEFEEE = MSVC debug fill for freed memory → **use-after-free pattern**

### Linker error landscape (dc3-decomp) — Updated counts (Agent D)
| Category | Count | Notes |
|----------|-------|-------|
| Unique unresolved symbols (total) | **867** | 334 non-string + 533 string literals |
| `lbl_` assembly labels | 195 | Split boundary gaps |
| `??__E` CRT constructors | 26 | **CRITICAL** -- nullified, leave globals uninitialized |
| SEH handlers (`__except_handler*`) | 48 | Low runtime impact |
| `merged_` symbols | 19 | Compiler-merged data |
| Third-party (libjpeg, curl, ogg, zlib) | 18 | Need stub .c files |
| String literals `??_C@` | 533 | COMDAT extraction broken in jeff |
| LNK4006 (duplicate symbol) | 411 | Harmless with /FORCE |
| LNK2013 (REL14 overflow) | 3 | Branch distance |

### COMDAT string failure root cause (Agent C)
Two-part failure:
1. **Jeff commit `cf01a80`** restricted COMDAT extraction to code-only → `.rdata` strings
   no longer get `IMAGE_COMDAT_SELECT_ANY` wrapping → linker can't deduplicate them
2. **Hash mismatch**: decomp compiler uses `A` hash for `??_C@` symbols, original binary
   uses full CRC hash (e.g., `JPIJPPAL`) → same string gets different symbol names
Fix: hash normalization in jeff's xex.rs (~15 lines of Rust)

### 26 nullified CRT constructors — what they leave uninitialized (Agent D)
| Symbol | Global | Risk |
|--------|--------|------|
| `??__ETheMemcardMgr@@YAXXZ` | TheMemcardMgr | **HIGH** -- memory allocation |
| `??__EThePresenceMgr@@YAXXZ` | ThePresenceMgr | **HIGH** -- network state |
| `??__ETheVirtualKeyboard@@YAXXZ` | TheVirtualKeyboard | **HIGH** -- UI state |
| `??__E?gThreadAchievements@@...` | Achievements::gThreadAchievements | **HIGH** -- vector |
| `??__E?sRoot@FilePath@@...` | FilePath::sRoot | MEDIUM -- path state |
| `??__E?sNull@FilePath@@...` | FilePath::sNull | MEDIUM -- path state |
| `??__EgDataPointMgr@@YAXXZ` | DataPointMgr::gDataPointMgr | MEDIUM |
| `??__EgWavMgr@@YAXXZ` | WavMgr::gWavMgr | MEDIUM |
| `??__EgChecksumData@@YAXXZ` | FileChecksum::gChecksumData | LOW |
| `??__EgEntries@@YAXXZ` | MessageTimer::gEntries | LOW |
| `??__EgOverride@@YAXXZ` | Keyboard::gOverride | LOW |
| 15x `??__Ek*ChunkID@@YAXXZ` | kListChunkID, kRiffChunkID, etc. | LOW -- 4-byte int constants |

These constructors exist in the asm .obj files at known MAP addresses (0x82A2E81C-0x82EE9D30)
but the decomp .obj replacements don't provide them → linker resolves to 0x82000000 → nullified
by CRT sanitizer → globals remain zero-initialized.

### ~110 NUI SDK constructors are safe to nullify (Agent B)
| Index Range | .obj Source | What |
|-------------|-----------|------|
| 98-114 | xspeechapi, facedetector | NUI speech + face detection (17) |
| 127 | sceneestimation | Scene estimation (1) |
| 133-165 | frontaldetector, multiposedetector | Face/pose detection (33) |
| 171-174 | speedmeasurement, Skeleton | Speed + skeleton vectors (4) |
| 272-306 | nuifitnesslib..nuiaudio | NUI fitness/face/head/audio (35) |
| 307-314 | NUISPEECH globals + COM | Speech globals + COM objects (8) |
| 319-330 | fecommon..rtcommon | COM arrays, D3D globals (12) |

These call unresolved internal NUI functions → execute PE header bytes → garbage results → heap corruption.
Safe to nullify since 62 NUI API functions are already stubbed.

### Repo states
| Repo | State | Key Issue |
|------|-------|-----------|
| **xenia** | 18+ files uncommitted on `headless-vulkan-linux` | Binary needs rebuild for bisect cvar |
| **dc3-decomp** | Working tree dirty, recent link with fixed jeff | 867 unresolved symbols |
| **jeff** | 9 files changed (BSS fix, COMDAT fix, CRT co-location) | Uncommitted |

## ORIENT — What does it mean?

### Critical path (updated)

```
XEX loads -> imports resolve -> NUI stubbed -> CRT init -> main() -> XAM load -> [NUI CALLBACK LOOP] -> App::Init -> ...
```

We are past `main()` and into the game's initialization sequence. The game is stuck in a
**NUI callback dispatch loop** — iterating a linked list of function pointers from the NUI
subsystem that were never properly initialized.

### Refined hypotheses (from Agent B + D convergence)

**Hypothesis A (CONFIRMED LIKELY): NUI SDK constructors call unresolved internal functions.**
~110 CRT constructors from NUI/Kinect SDK libraries (indices 98-330) initialize complex COM/ATL
objects, private heaps (`g_heap@NUISPEECH`), D3D globals (`g_RTGlobal`). These call internal SDK
functions NOT among the 62 stubbed NUI API functions. Those internal functions resolve to 0x82000000
(RODATA/PE header) via `/FORCE:UNRESOLVED` and "execute" as garbage PPC code, producing corrupt
return values that overwrite heap metadata.

**Hypothesis B (CONFIRMED LIKELY): 26 nullified constructors leave critical globals uninitialized.**
TheMemcardMgr, ThePresenceMgr, TheVirtualKeyboard, gThreadAchievements, FilePath::sRoot etc.
remain zero-initialized. Later CRT constructors (or `_cinit()` itself) dereference these null globals,
causing use-after-free (0xFEEEFEEE pattern) when they touch freed/invalid heap memory.

**Both hypotheses are likely active simultaneously.** NUI constructors corrupt heap metadata directly,
AND uninitialized globals cause indirect corruption when later code uses them.

### What's NOT blocking us
- NUI API stubbing: working (62/62)
- Import resolution: working (707/707)
- JIT compilation: working (arena + label fixes applied)
- CRT sanitizer: working (26/390 nullified, correct threshold)
- RODATA workaround: working (still needed)
- Xenia Linux platform: working (QueryProtect, SIGSEGV handler, MMIO all fixed)
- MAP address extraction: validated, production-ready (70/70 match)

## DECIDE — What's the highest-leverage fix?

**Priority 1 (NOW): Break the NUI callback dispatch loop.**
The game is stuck calling garbage function pointers in a linked list at guest 0x834B1F40.
Options: (a) stub the dispatch function itself to skip the loop, (b) patch the linked list
to be empty (sentinel == head), (c) add a call-count limiter for no-op stub targets.

**Priority 2: Identify what game code runs after the callback loop exits.**
Once the loop is broken, observe the next crash/hang and iterate.

**Priority 3: Fix the 26 missing CRT constructors in dc3-decomp/jeff.**
The 11 complex constructors (TheMemcardMgr, etc.) need their `??__E` symbols exported from
the asm .obj files, or the decomp source must define proper C++ initializers.

**Priority 4: Fix COMDAT string extraction in jeff.**
Hash normalization in xex.rs to resolve 533 unresolved `??_C@` string literals.

**Priority 5 (ONGOING): Reduce remaining 195 lbl_ + 48 SEH + 18 third-party symbols.**

## ACT — See TODO.md

---

### Session 11 (2026-02-23): DC3 NUI/XBC Resolver + Guest Override Validation (in progress)
- Verified local Xenia build system on this repo: `./xb build` (Make-based), not a required Ninja flow
- Built `xenia-headless` successfully after guest override + resolver changes
- Ran timed headless smoke on `/home/free/code/milohax/dc3-decomp/build/373307D9/default.xex`
- Confirmed manifest-first NUI/XBC resolver is active with `--stub_nui_functions=true`
  - `manifest_hits=63`, `symbol_hits=0`, `catalog_hits=1` (64 total NUI/XBC patch specs)
  - `Registered 64 guest extern overrides...`
  - `NUI patch/override summary: patched=0 overridden=64 skipped=0`
- Confirmed direct guest-call extern path works in practice (no byte patching needed for eligible simple stubs)
- Found `.text` fingerprint mismatch between manifest hash and Xenia runtime hash
  - Manifest currently hashes static PE `.text`
  - Xenia logs runtime `.text` bytes after module load (different hash in this build)
  - Added fallback: when an explicit manifest path is provided, trust `build_label` for layout selection and log mismatch
- Extracted DC3 NUI/XBC resolver/parsing code out of `emulator.cc` into:
  - `src/xenia/dc3_nui_patch_resolver.h`
  - `src/xenia/dc3_nui_patch_resolver.cc`
- Added manifest parser support for optional runtime fingerprint field:
  - `pe.text.xenia_runtime_fnv1a64` (preferred for layout matching when present)
- Updated dc3-decomp manifest generator to emit explicit fingerprint semantics:
  - `pe.text.fnv1a64` (artifact/static hash currently used by generator)
  - `pe.text.fnv1a64_static_pe`
  - optional `pe.text.xenia_runtime_fnv1a64`
- Added signature resolver implementation (strict/hybrid path) for:
  - Cross-build (original + decomp): `NuiInitialize`, `NuiShutdown`, `NuiMetaCpuEvent`, `CXbcImpl::{Initialize,DoWork,SendJSON}`
  - Decomp-only (currently): `D3DDevice_Nui{Initialize,Start,Stop}`
  - unique-match + validation + local hint window around catalog addresses
- Added `dc3_nui_signature_trace` cvar to dump runtime PPC words for signature-backed targets
  - Used to retune decomp signatures from real runtime patch-site bytes (not function-start symbols)
- Signature resolver hardening + current status:
  - Resolver now uses local signature hints to adjust manifest/symbol targets to actual patch sites (for known signature-backed targets)
  - Current decomp hybrid manifest run resolves all entries directly from manifest in this branch state:
    - `manifest_hits=85`, `signature_hits=0`, `catalog_hits=0` (85 total)
    - `Registered 85 guest extern overrides...`
  - Current original hybrid run (with decomp manifest still auto-probed) falls back cleanly to symbols/catalog:
    - `manifest_hits=0`, `symbol_hits=56`, `catalog_hits=3` (59 total)
    - `Registered 59 guest extern overrides...`
  - Strict/signature-only smoke (no manifest/symbols, `--dc3_nui_enable_signature_resolver=true`) now resolves:
    - decomp: `85/85` (`signature_hits=85`, `strict_rejects=0`)
    - original: `59/59` (`signature_hits=59`, `strict_rejects=0`)
  - Current strict signature-covered targets:
    - Full current DC3 NUI/XBC patch tables in this branch state:
      - original: all `59/59`
      - decomp: all `85/85` (including `Nuip*`, `D3DDevice_Nui*`, speech/fitness/wave/head, camera/identity)
  - Resolver remains fail-closed on local hint misses and ambiguous matches (no unsafe global clone fallback)
- Added `xenia-core-tests` suite with resolver unit tests (Catch2):
  - hex parser / fingerprint cache parser / manifest parser (runtime fingerprint field)
  - strict signature resolve, strict fail-closed behavior
  - strict ambiguity rejection (global multi-match) and local-miss/global-clone fail-closed behavior
  - speech-family ambiguity regression (orig+decomp `NuiSpeechEnable` variants duplicated -> reject)
  - hybrid manifest-target adjustment via local signature hint
  - synthetic coverage matrix for current signature-backed targets (original + decomp variants as supported)
  - Current filtered run: `All tests passed (648 assertions in 10 test cases)`
- Runtime-validated decomp fingerprint for Xenia layout cache: `0x28F813763596C6C6`
- NUI/XBC cutover defaults promoted in `emulator.cc`:
  - `dc3_guest_overrides=true` (default-on; legacy NUI/XBC byte-patch fallback removed)
  - `dc3_nui_enable_signature_resolver=true` (default on)
  - `dc3_nui_patch_resolver_mode` remains `hybrid` (default)
- Cutover validation matrix (2026-02-23):
  - Default path, original XEX:
    - `guest_overrides=1`, `resolver_mode=hybrid`
    - `Registered 59 guest extern overrides`
    - `patched=0 overridden=59 skipped=0 total=59`
  - Default path, decomp XEX:
    - `guest_overrides=1`, `resolver_mode=hybrid`
    - `Registered 85 guest extern overrides`
    - `patched=0 overridden=85 skipped=0 total=85`
  - Strict signatures-only (no manifest/symbols), original:
    - `signature_hits=59`, `strict_rejects=0`
    - `patched=0 overridden=59 skipped=0 total=59`
  - Strict signatures-only (no manifest/symbols), decomp:
    - `signature_hits=85`, `strict_rejects=0`
    - `patched=0 overridden=85 skipped=0 total=85`
  - Conclusion:
    - DC3 NUI/XBC is cut over to resolver+guest-override by default.
    - Follow-up validation cleared removal of the legacy DC3 NUI/XBC byte-patch path.
- Legacy-removal roadmap (DC3 NUI/XBC only; not CRT/skeleton behavior hacks, now completed):
  1. Add a cutover gate script / CI check that runs `original+decomp` in `hybrid` and `strict` and asserts `patched=0`, `unresolved=0`.
  2. Keep legacy fallback path for one validation window while collecting logs from real workflow runs. (Done)
  3. Remove DC3 NUI/XBC byte-write application for entries fully handled by guest overrides (keep non-NUI legacy hacks separate). (Done)
  4. Delete `dc3_guest_override_poc` alias and update docs/scripts to `dc3_guest_overrides`. (Done)
  5. Reassess whether `resolver_mode=strict` can become the default after manifest/symbol workflows are stable. (Open)
- Cutover gate automation added:
  - `tools/dc3_nui_cutover_gate.sh`
  - Runs original+decomp across:
    - default (`hybrid` + guest overrides)
    - strict signatures-only (manifest/symbol disabled)
  - Asserts resolver/apply summaries and `TIMEOUT` completion.
  - Current result: passed twice consecutively (deterministic) on local runs.
- Legacy DC3 NUI/XBC byte-patch application path removed after cutover validation:
  - NUI/XBC now uses a single apply path: resolver -> guest extern override registration
  - `dc3_guest_overrides=false` now warns and is forced on (legacy byte patching removed)
  - Post-removal validation:
    - `tools/dc3_nui_cutover_gate.sh` (default+strict) passed on original + decomp
    - `xenia-core-tests \"[dc3_nui_patch_resolver]\"` passed (`648 assertions / 10 test cases`)
  - Rollback path for legacy byte patching: revert commit `411064457` (checkpoint before removal) or the legacy-removal commit itself.

---

## History

### Sessions 14-15 (2026-02-23): JIT Crash Cascade Fixed — Game Fully Stable

**Problem**: Game crashed between 3-6s during JIT compilation of zero-filled stub functions.
Cascading failures: null Function* in CALL emitter, constant-as-register in CALL_INDIRECT,
null machine_code pointers, null indirection table entries, SIGBUS on `movbe`, out-of-bounds
HIR builder arena allocation, and unmapped guest memory reads.

**Fixes applied** (10 files, ~20 individual changes):

1. **SIGBUS handler** (exception_handler_posix.cc): Added SIGBUS to signal handler registration.
   Linux sends SIGBUS (not SIGSEGV) for file-backed mmap accesses past the backing file size.
   Xenia's MMIO handler now sees these faults.

2. **Soft fault handler** (mmio_handler.cc): For unmapped guest memory reads (stack guard pages,
   unmapped regions), zero the destination register and advance past the faulting instruction
   instead of crashing. Uses existing TryDecodeLoadStore mechanism.

3. **CALL null guard** (x64_seq_control.cc): Skip CALL when Function* is null.
   Also added null guards to all 6 CALL_TRUE variants.

4. **CALL_INDIRECT constant handling** (x64_seq_control.cc): When I64Op target is a constant,
   load into rax and call through that. Skip if constant is 0. Covers all CALL_INDIRECT_TRUE.

5. **XObject::GetNativeObject tolerance** (xobject.cc): Changed assert_always to XELOGW
   for unknown object types.

6. **Null machine_code guard** (x64_function.cc): Return false from CallImpl when
   machine_code_ is null.

7. **Null indirection table entries** (x64_emitter.cc): After loading from indirection table,
   check for 0 and redirect to resolve function thunk. Both Call() and CallIndirect().

8. **PPC scanner bounds check** (ppc_scanner.cc): Added max_scan_address using
   QueryBaseAndSize to prevent scanning past mapped memory.

9. **HIR builder bounds check** (ppc_hir_builder.cc): Cap function size to 1MB, compute
   instr_count from capped size (before arena allocation), clamp end_address to mapped memory.

10. **Guard page pre-mapping** (dc3_hack_pack.cc): Map 0x7F000000 (16MB GPU writeback) and
    0xFFFFF000 (top-of-address-space guard) as anonymous readable pages at startup.
    Eliminates ~807K faults/run → only 2 remaining.

**Thread status reporting fix** (emulator_headless.cc): Run() now delegates to RunWithTimeout(-1)
for continuous reporting. Enhanced JIT IP sampling to 3 samples 50ms apart for loop detection.

**Progression**: test52 (CALL crash) → test54 (CALL_INDIRECT assert) → test55 (XObject assert) →
test57 (null machine_code) → test59 (HIR builder crash) → test62 (SIGBUS) → test63 (13 reports,
game alive!) → test65 (soft faults, 807K) → test70 (guard pages, 112 faults) → test72 (2 faults)

**Current state**: Game runs full 40s timeout, EXIT 0, 2 handled faults, Thread 6 in
memory scanning loop at 0x834AA920, no GPU commands yet.

### Sessions 12-13 (2026-02-23): JIT Indirect Call Fix — Game Runs Stably
- Fixed SIGSEGV at host 0x40002830: XAM COM vtable indirect call outside indirection table range
- Added bounds check in `CallIndirect` (x64_emitter.cc) with resolve thunk fallback
- Fixed volatile register clobber: slow path was calling ResolveFunction as C function from JIT
- Added no-op return stub for unresolvable functions (null target, XAM, garbage pointers)
- Added bounds checks in `AddIndirection` and `PlaceGuestCode` (x64_code_cache.cc)
- Fixed Linux SysV ABI register ordering bug in CallIndirect
- Made `kIndirectionTableBase`/`kIndirectionTableSize` public in x64_code_cache.h
- **Result**: Game runs full 30-second timeout, exit code 124, zero crashes
- 4.4MB of log output, game is executing game code
- New blocker: NUI callback dispatch loop at 0x834B1F40 (garbage function pointers)
- Created `docs/dc3-boot/DEBUGGING_TIPS.md` capturing JIT architecture knowledge

### Session 11 (2026-02-23): NUI/XBC Resolver + Guest Override Validation
- (See detailed notes above in "2026-02-23 Update" section)

### Session 10 (2026-02-22): Heap Corruption SOLVED
- Wave 1: 5 parallel research agents completed (see docs/dc3-boot/agent_*.md)
- Fixed bisect cvar (`dc3_crt_bisect_max`): changed default from 0 to -1, fixed `>0` to `>=0`
- Fixed bisect script detection: check for crash address 0x830DC644 in log, not function name
- Fixed bisect script: xenia catches SIGSEGV internally, keeps running until timeout
- **Binary search result: Constructor #69 (0x82A9C138) is first heap corruption culprit**
  - Confirmed deterministic: 3/3 repros at N=69, 0/3 at N=68
  - Constructor 69 PPC code: initializes global at 0x83B33850 by calling function at 0x8355D730
- Implemented `--dc3_crt_skip_indices` cvar for targeted constructor exclusion
- Skipping #69 alone insufficient — NUI range 98-330 also contains culprit(s)
- **Skipping #69 + NUI 98-330 eliminates heap corruption entirely**
  - Before: RtlEnterCS=186, RtlInitCS=10 → heap crash (Sord + FEEEFEEE)
  - After: RtlEnterCS=276, RtlInitCS=13 → null vtable crash (0x830E3340)
- New crash: null vtable at 0x830E3340 — lwz r9, 4(r11) where r11=0
  - Object's vtable pointer is NULL from uninitialized global
  - This is expected: 26 nullified CRT constructors leave critical globals zeroed
- MAP was overwritten by Agent D link run (11:23 timestamp ≠ XEX 05:20 timestamp)
  - Need to rebuild dc3-decomp for consistent MAP+XEX pair

### Session 9 (2026-02-22): Clean Rebuild + Address Regeneration
- Clean rebuild of dc3-decomp with fixed jeff toolchain
- LNK4006 errors dropped 3,351 -> 411, LNK2013 dropped 17 -> 3
- Removed zero page mapping (0x0-0x10000) to surface null-pointer crashes
- Regenerated ALL 62 NUI patch addresses, Xapi stubs, CRT table bounds from MAP
- Fixed arena.cc oversized chunk assertion (guest functions > 4MB)
- Fixed finalization_pass.cc label ID overflow (functions with > 65535 blocks)
- CRT constructors executing: 320 nullified -> only 26 nullified (364/390 now run)
- Crash: heap corruption in RtlpCoalesceFreeBlocks ("Sord" + 0xFEEEFEEE pattern)

### Sessions 1-8: See docs/DC3_HEADLESS_CHANGE_AUDIT_2026-02-20.md
