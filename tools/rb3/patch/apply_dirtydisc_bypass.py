#!/usr/bin/env python3
"""Apply the RB3 dirty-disc / ARK-checksum bypass to clean-TU5 XEXes.

RB3Enhanced (source/rb3enhanced.c ApplyPatches) neutralises RB3's content
integrity check by overwriting PlatformMgr::SetDiskError with a single BLR:

    // Patch out PlatformMgr::SetDiskError - this effectively nullifies
    // checksum checks on ARKs and MIDs.
    POKE_32(PORT_SETDISKERROR, BLR);   // ports_xbox360.h: 0x82516320

SetDiskError(this, code) stores `code` into this+0x34 and, when non-zero, walks
into the disk-error flow that ultimately raises XamShowDirtyDiscErrorUI (the
"ShowDirtyDiscAndBail" helper at 0x8283D740). Making the function return
immediately means the error state is never set, so the dirty-disc UI is never
triggered and boot proceeds. This is RB3E's proven, community-standard bypass
and targets the exact same retail TU5 v0.0.5.1 as clean_tu5.xex.

clean_tu5.xex stores its basefile FLAT (compression=0, encryption=0), so
VA->file_off = 0x3000 + (VA - 0x82000000). Byte-compatible with the
same-instrument writes (disjoint regions).
"""
import struct, sys

IMAGE_BASE, PE_OFF = 0x82000000, 0x3000
SETDISKERROR_VA = 0x82516320
BLR = 0x4E800020
MFLR_R12 = 0x7D8802A6  # first insn of SetDiskError, the 'old' value we validate

def off(va):
    return PE_OFF + (va - IMAGE_BASE)

def patch(src, dst):
    data = bytearray(open(src, "rb").read())
    o = off(SETDISKERROR_VA)
    cur = struct.unpack_from(">I", data, o)[0]
    if cur == BLR:
        print(f"  {src}: already BLR at {SETDISKERROR_VA:#x} (idempotent)")
    elif cur != MFLR_R12:
        sys.exit(f"OLD-MISMATCH at {SETDISKERROR_VA:#x} (off {o:#x}): "
                 f"expect {MFLR_R12:#x} got {cur:#x}")
    struct.pack_into(">I", data, o, BLR)
    open(dst, "wb").write(data)
    got = struct.unpack_from(">I", bytearray(open(dst, "rb").read()), o)[0]
    assert got == BLR, f"post-write check failed: {got:#x}"
    print(f"OK: {dst}  SetDiskError@{SETDISKERROR_VA:#x} -> BLR (off {o:#x})")

if __name__ == "__main__":
    base = "/home/free/code/milohax/rb3-xenon/_tu5probe/clean"
    # 1. bypass only
    patch(f"{base}/clean_tu5.xex",         f"{base}/clean_tu5_nodd.xex")
    # 2. bypass + same-instrument (clean_tu5_patched.xex already carries SI writes)
    patch(f"{base}/clean_tu5_patched.xex", f"{base}/clean_tu5_nodd_siPATCH.xex")
