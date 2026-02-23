# Goal: Boot Dance Central 3 Decomp in Xenia on Linux

## Success Criteria

1. **DC3 decomp XEX loads and reaches `main()`** — CRT static initialization completes without heap corruption
2. **DC3 decomp XEX reaches `App::Init()`** — game initialization begins, resource loading starts
3. **DC3 decomp XEX renders a frame** — Vulkan GPU backend produces visible output
4. **DC3 decomp XEX reaches menu** — game is interactive

## Repos Involved

| Repo | Path | Role |
|------|------|------|
| **xenia** | `~/code/milohax/xenia` | Xbox 360 emulator (this repo) |
| **dc3-decomp** | `~/code/milohax/dc3-decomp` | DC3 decompilation project |
| **jeff** | `~/code/milohax/jeff` | Custom dtk/COFF toolchain for dc3-decomp |

## Process: Recursive OODA Loop

Each iteration follows the same cycle:

```
OBSERVE  →  What happens when we run the test?
ORIENT   →  What does it mean? What's the root cause?
DECIDE   →  What's the highest-leverage fix?
ACT      →  Implement the fix, rebuild, test again
```

### How to iterate

1. Read `STATUS.md` for current observations and blockers
2. Read `TODO.md` for current actionable tasks
3. Execute the highest-priority TODO items
4. Rebuild (dc3-decomp and/or xenia as needed)
5. Run the boot test
6. Update `STATUS.md` with new observations
7. Update `TODO.md` — check off completed items, add new ones
8. Repeat from step 1

### Build Commands

```bash
# DC3-Decomp rebuild
cd ~/code/milohax/dc3-decomp && ninja link 2>&1 | tee /tmp/link.txt && python3 scripts/build/build_xex.py

# Xenia rebuild
cd ~/code/milohax/xenia/build && make xenia-headless config=checked_linux -j$(nproc)

# Test
rm -f /dev/shm/xenia_* 2>/dev/null
~/code/milohax/xenia/build/bin/Linux/Checked/xenia-headless \
    --gpu=null --target=~/code/milohax/dc3-decomp/build/373307D9/default.xex \
    --headless_timeout_ms=20000 2>&1 | tee /tmp/test.log
```

### Concurrency Strategy

These repos have independent build pipelines. Many analysis tasks are pure reads.
When working across repos, launch parallel subagents:

- **xenia agents**: Instrument emulator, fix runtime crashes, update patch addresses
- **dc3-decomp agents**: Analyze MAP/symbols, fix splits, stub missing functions
- **jeff agents**: Fix COFF generation bugs, improve symbol handling

Agents that only read files can always run in parallel. Agents that write files
within the same repo must be sequenced.

## Key Files

### Xenia
| File | Purpose |
|------|---------|
| `src/xenia/emulator.cc` | DC3 patches: NUI stubs, CRT sanitizer, RODATA workaround |
| `src/xenia/app/emulator_headless.cc` | Headless main loop, diagnostic thread |
| `src/xenia/base/arena.cc` | JIT arena allocator (oversized chunk fix) |
| `src/xenia/cpu/compiler/passes/finalization_pass.cc` | JIT label ID fix |
| `docs/DC3_HEADLESS_CHANGE_AUDIT_2026-02-20.md` | Full session audit trail |

### DC3-Decomp
| File | Purpose |
|------|---------|
| `build/373307D9/default.map` | Address lookups for all symbols |
| `build/373307D9/default.xex` | Decomp XEX under test |
| `config/373307D9/splits.txt` | Symbol split boundaries |
| `config/373307D9/objects.json` | Object matching status |

### Jeff
| File | Purpose |
|------|---------|
| `src/util/xex.rs` | COFF generation (BSS fix, COMDAT fix applied) |
| `src/util/split.rs` | CRT initializer co-location |
