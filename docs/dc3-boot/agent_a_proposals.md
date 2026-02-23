# Agent A: Heap Canary Instrumentation Proposals

*Date: 2026-02-22*

## Context

The binary search script (`tools/dc3_crt_bisect.sh`) finds *which* constructor
index triggers the crash, but it treats each test as a black box (crash vs no
crash). Adding heap canary checks inside the emulator would give us *when*
during execution the corruption actually happens, and could catch corruption
that doesn't immediately crash.

## Proposal 1: Post-CRT-entry canary check in emulator.cc

### Concept

After the CRT table sanitization loop (emulator.cc ~line 1677), but before
`LaunchModule()`, write a known canary pattern to a small region of the guest
heap. Then, after each CRT constructor returns, verify the canary is intact.

This would require modifying how `_cinit()` iterates the constructor table.
Since `_cinit()` is *guest code* compiled into the XEX, we can't easily hook
individual constructor calls from the emulator side. However, we have two
options:

### Option A: Patch the guest _cinit loop (PREFERRED)

The guest `_cinit()` function iterates the `__xc_a` to `__xc_z` table and
calls each non-null entry. We know its address from the MAP file. We could:

1. Before launching the module, allocate a small guest heap region (e.g. 64
   bytes) via `RtlAllocateHeap` (or directly via the memory system).
2. Write a canary pattern: `0xDEADCAFE` repeated 16 times.
3. Patch the guest `_cinit` function to, after each `blr` return from a
   constructor, call a small trampoline that verifies the canary.

**Complexity**: High. Requires writing PPC assembly for the trampoline and
patching guest code. Not recommended for initial investigation.

### Option B: Bracket bisect with heap validation (RECOMMENDED)

Instead of patching guest code, use the existing bisect infrastructure more
cleverly. After the CRT table is sanitized but before the module launches:

1. Record the state of a few known heap metadata locations.
2. After the module crashes (or times out), check those locations in the
   diagnostic thread.

**Specific code changes for emulator.cc** (around line 1677, after the CRT
sanitizer block):

```cpp
// --- BEGIN PROPOSED ADDITION ---
// DC3 heap canary: Write a sentinel pattern into a fresh heap allocation
// so we can detect when heap metadata gets corrupted.
// This runs BEFORE the game thread starts, so the heap should be pristine.
if (cvars::dc3_crt_bisect_max > 0) {
    // Allocate a small block from the guest default process heap.
    // The Xbox 360 default process heap is at a known address; we can
    // also find it via kernel_state_->process_info_block().
    //
    // We'll write canaries AROUND a heap allocation to detect both
    // underflow and overflow corruption.
    //
    // Strategy: allocate 256 bytes, then check the 16 bytes before
    // and after the allocation (heap metadata) periodically.

    uint32_t canary_guest_addr = 0;  // Will be set by RtlAllocateHeap
    // Use the kernel's heap allocator to get a real heap block
    auto* heap_ptr = memory_->TranslateVirtual<uint8_t*>(
        kernel_state_->process_info_block_address() + 0x18);  // DefaultProcessHeap
    uint32_t process_heap = xe::load_and_swap<uint32_t>(heap_ptr);

    if (process_heap != 0) {
        // Call RtlAllocateHeap(process_heap, 0, 256) via kernel shim
        // Store the returned pointer as canary_guest_addr
        // Then write: 0xCAFECAFE pattern into the allocation
        // The heap metadata before/after this block is what we monitor.

        XELOGI("DC3: Heap canary installed at guest address {:08X}, "
               "process_heap={:08X}", canary_guest_addr, process_heap);
    }
}
// --- END PROPOSED ADDITION ---
```

**Problem with Option B**: We don't have a clean way to call `RtlAllocateHeap`
from the emulator host side before the guest thread starts, because the kernel
shim expects to be called from a guest thread context.

### Option C: Monitor heap free-list head (ACTUALLY RECOMMENDED)

The simplest and most effective approach. Instead of allocating anything, just
snapshot the heap's internal metadata (free list head, segment list, etc.) and
check it periodically from the diagnostic thread.

**Specific code changes for emulator.cc** (after CRT sanitizer, ~line 1677):

```cpp
// DC3 heap monitor: snapshot heap metadata before CRT constructors run.
// The Xbox 360 default process heap structure (RTL_HEAP) has known offsets:
//   +0x000: Signature (should be 0xFFEEFFEE or similar)
//   +0x014: Flags
//   +0x058: FreeLists (LIST_ENTRY)
//   +0x0C0: LargeBlocksIndex
//
// We snapshot these BEFORE the game thread starts (heap is pristine).
// The diagnostic thread in emulator_headless.cc can then compare.

static uint32_t s_dc3_heap_base = 0;
static uint32_t s_dc3_heap_signature = 0;
static uint32_t s_dc3_heap_freelist_flink = 0;
static uint32_t s_dc3_heap_freelist_blink = 0;

if (cvars::dc3_crt_bisect_max > 0) {
    // Read the default process heap address from the PEB/process info block.
    // For the DC3 decomp, we can also hardcode this from a previous run's logs.
    // TODO: Read from process_info_block_address() + offset

    uint32_t heap_base = /* read from PEB or hardcode from MAP */;
    if (heap_base != 0) {
        auto* hp = memory_->TranslateVirtual<uint8_t*>(heap_base);
        s_dc3_heap_base = heap_base;
        s_dc3_heap_signature = xe::load_and_swap<uint32_t>(hp + 0x00);
        s_dc3_heap_freelist_flink = xe::load_and_swap<uint32_t>(hp + 0x58);
        s_dc3_heap_freelist_blink = xe::load_and_swap<uint32_t>(hp + 0x5C);

        XELOGI("DC3: Heap metadata snapshot: base={:08X} sig={:08X} "
               "freelist=[{:08X},{:08X}]",
               heap_base, s_dc3_heap_signature,
               s_dc3_heap_freelist_flink, s_dc3_heap_freelist_blink);
    }
}
```

**Corresponding check in emulator_headless.cc** (in the periodic diagnostic
loop, around line 162, inside the 3-second report):

```cpp
// DC3 heap canary check
extern uint32_t s_dc3_heap_base;
extern uint32_t s_dc3_heap_signature;
extern uint32_t s_dc3_heap_freelist_flink;

if (s_dc3_heap_base != 0) {
    auto* hp = emulator_->memory()->TranslateVirtual<uint8_t*>(s_dc3_heap_base);
    uint32_t current_sig = xe::load_and_swap<uint32_t>(hp + 0x00);
    uint32_t current_flink = xe::load_and_swap<uint32_t>(hp + 0x58);

    bool corrupted = false;
    if (current_sig != s_dc3_heap_signature) {
        fprintf(stderr, "DC3 HEAP CORRUPT: signature changed %08X -> %08X\n",
                s_dc3_heap_signature, current_sig);
        corrupted = true;
    }
    // Check for obviously invalid free list pointers
    if (current_flink != 0 && (current_flink < 0x70000000 || current_flink > 0x80000000)) {
        fprintf(stderr, "DC3 HEAP CORRUPT: freelist flink=%08X (out of heap range)\n",
                current_flink);
        corrupted = true;
    }
    // Check for the known corruption pattern
    // Scan first 256 bytes of heap header for "Sord" (0x536F7264) or 0xFEEEFEEE
    if (!corrupted) {
        for (int i = 0; i < 256; i += 4) {
            uint32_t val = xe::load_and_swap<uint32_t>(hp + i);
            if (val == 0x536F7264 || val == 0xFEEEFEEE) {
                fprintf(stderr, "DC3 HEAP CORRUPT: found pattern %08X at heap+%d\n",
                        val, i);
                corrupted = true;
                break;
            }
        }
    }
    if (corrupted) {
        fprintf(stderr, "DC3 HEAP: dumping first 128 bytes:\n");
        for (int i = 0; i < 128; i += 16) {
            fprintf(stderr, "  +%03X:", i);
            for (int j = 0; j < 16; j += 4) {
                fprintf(stderr, " %08X", xe::load_and_swap<uint32_t>(hp + i + j));
            }
            fprintf(stderr, "\n");
        }
    }
}
```

## Proposal 2: Per-constructor heap check via bisect (NO CODE CHANGES)

Instead of modifying the emulator, we can use the bisect script more
aggressively. The binary search finds the *first* corrupting constructor.
But what if multiple constructors each corrupt the heap?

**Extended bisect approach:**

1. Run binary search to find first corrupting index K.
2. Nullify constructor K (modify the bisect flag to skip specific indices).
3. Re-run binary search to find the next corrupting index.
4. Repeat until no more crashes.

This would require a new cvar like `--dc3_crt_skip_indices=K1,K2,...` that
nullifies specific indices regardless of bisect_max. This is a small code
change (~15 lines in emulator.cc) and would be very valuable if there are
multiple corrupting constructors.

**Proposed cvar addition to emulator.cc** (around line 74):

```cpp
DEFINE_string(dc3_crt_skip_indices, "",
              "DC3: comma-separated list of CRT constructor indices to "
              "nullify (for targeted exclusion after binary search).",
              "General");
```

**And in the CRT sanitizer loop** (around line 1659):

```cpp
// Parse skip list once
static std::set<int> skip_set;
if (skip_set.empty() && !cvars::dc3_crt_skip_indices.empty()) {
    std::istringstream ss(cvars::dc3_crt_skip_indices);
    std::string token;
    while (std::getline(ss, token, ',')) {
        skip_set.insert(std::stoi(token));
    }
}

// Inside the loop, after the bisect_max check:
} else if (!skip_set.empty() && skip_set.count(index)) {
    XELOGI("DC3: CRT[{:3d}] = {:08X} (nullified-skip, in skip list)",
            index, entry);
    xe::store_and_swap<uint32_t>(p, 0);
    nullified_skip++;
```

## Recommendation

**Start with the bisect script alone** (Phase 1). It requires zero emulator
changes and will identify the first corrupting constructor index within
~9 iterations (log2(389) ~ 8.6), taking about 3 minutes total.

If the crash is deterministic and a single constructor is responsible, the
bisect script output plus the MAP file will directly identify the culprit
function.

If the crash is flaky or involves multiple constructors, then implement
**Proposal 2** (the `--dc3_crt_skip_indices` cvar) to iteratively exclude
known-bad constructors and continue searching.

**Option C** (heap metadata monitoring) is valuable for understanding *what*
gets corrupted, but is lower priority than finding *which constructor* does it.
Implement it only if the bisect result is ambiguous.
