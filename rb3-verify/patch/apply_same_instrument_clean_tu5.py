#!/usr/bin/env python3
"""Apply the RB3 same-instrument patch to the uncompressed clean-TU5 XEX.

clean_tu5.xex stores its basefile as a FLAT image (compression=0, encryption=0),
so VA->file_off = pe_off(0x3000) + (VA - image_base(0x82000000)). The writes list
(default_tu5_patched.writes.json: 671 cave words + 4 detours) is the same one that
targets RB3DX (byte-identical at all patch VAs + cave per the divergence doc).
Validates every 'old' value before mutating; verifies detours become branches and
gSameInstrumentEnabled@0x82C8AAA0==1 afterwards.
"""
import json, struct, sys
WRITES = "/home/free/code/milohax/rb3-xenon/.claude/worktrees/tu5-migrate/orig/45410914/default_tu5_patched.writes.json"
SRC = "/home/free/code/milohax/rb3-xenon/_tu5probe/clean/clean_tu5.xex"
DST = "/home/free/code/milohax/rb3-xenon/_tu5probe/clean/clean_tu5_patched.xex"
PE_OFF, IMAGE_BASE = 0x3000, 0x82000000
def off(va): return PE_OFF + (va - IMAGE_BASE)
W = json.load(open(WRITES)); data = bytearray(open(SRC, "rb").read())
for x in W:
    o, old = off(int(x["va"], 16)), int(x["old"], 16)
    cur = int.from_bytes(data[o:o+4], "big")
    if cur != old: sys.exit(f"OLD-MISMATCH {x['va']} {x['class']} expect {old:#x} got {cur:#x}")
for x in W:
    struct.pack_into(">I", data, off(int(x["va"], 16)), int(x["new"], 16))
open(DST, "wb").write(data)
rd = lambda o: int.from_bytes(data[o:o+4], "big")
for va in (0x826684C0, 0x825B6488, 0x8276FA08, 0x82794740):
    assert (rd(off(va)) >> 26) == 18, f"detour {va:#x} not a branch"
assert rd(off(0x82C8AAA0)) == 1, "gSameInstrumentEnabled != 1"
print("OK: patched, 4 detours are branches, gSameInstrumentEnabled=1")
