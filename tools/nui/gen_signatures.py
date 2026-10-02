#!/usr/bin/env python3
"""Generate a NUI SDK signature table (src/xenia/kernel/nui/nui_sdk_sigs_<ver>.inc)
from a dtk split listing of a title that statically links that SDK version.

Usage:
  tools/nui/gen_signatures.py <asm-dir> <version> <out.inc>
  e.g. tools/nui/gen_signatures.py ../dc3-decomp/build/373307D9/asm 2.0.21173 \
       src/xenia/kernel/nui/nui_sdk_sigs_2_0_21173.inc

Each API function gets a masked word signature (OOVPA-style):
  - every instruction word of the function, up to MAX_WORDS;
  - relocated fields masked: @ha/@h/@l immediates (0xFFFF), and the target
    field of a branch to a named symbol (I-form 0x03FFFFFC, B-form 0xFFFC).
    Branches to local .L_ labels are position-independent and kept.
If the masked body is not unique among every function start in the listing
(thunks such as `lis/addi/b CNuiSpeechAPI::X` differ only in their target),
the signature gets an XREF anchor: the tail branch's target must match that
target function's own masked signature. Still ambiguous -> the generator
fails; it never emits a signature it cannot show unique on the reference.

No address and no title id is emitted: the table is keyed by the SDK version
only. The runtime resolver additionally refuses zero or multiple matches over
the whole .text of the running image.
"""
import os
import re
import sys

MAX_WORDS = 24
TARGET_WORDS = 16

# The NUI SDK entry points a title calls (the public API). Order is the
# facade's table order.
API = [
    "NuiInitialize", "NuiShutdown",
    "NuiSkeletonTrackingEnable", "NuiSkeletonTrackingDisable",
    "NuiSkeletonSetTrackedSkeletons", "NuiSkeletonGetNextFrame",
    "NuiImageStreamOpen", "NuiImageStreamGetNextFrame",
    "NuiImageStreamReleaseFrame",
    "NuiImageGetColorPixelCoordinatesFromDepthPixel",
    "NuiAudioCreate", "NuiAudioCreatePrivate", "NuiAudioRegisterCallbacks",
    "NuiAudioUnregisterCallbacks", "NuiAudioRegisterCallbacksPrivate",
    "NuiAudioUnregisterCallbacksPrivate", "NuiAudioRelease",
    "NuiCameraSetProperty", "NuiCameraGetProperty", "NuiCameraGetPropertyF",
    "NuiCameraSetExposureRegionOfInterest",
    "NuiCameraGetExposureRegionOfInterest", "NuiCameraElevationSetAngle",
    "NuiCameraElevationGetAngle", "NuiCameraAdjustTilt",
    "NuiCameraGetNormalToGravity",
    "NuiIdentityEnroll", "NuiIdentityIdentify",
    "NuiIdentityGetEnrollmentInformation", "NuiIdentityAbort",
    "NuiFitnessStartTracking", "NuiFitnessPauseTracking",
    "NuiFitnessResumeTracking", "NuiFitnessStopTracking",
    "NuiFitnessGetCurrentFitnessData",
    "NuiWaveSetEnabled", "NuiWaveGetGestureOwnerProgress",
    "NuiHeadOrientationDisable", "NuiHeadPositionDisable",
    "NuiSpeechEnable", "NuiSpeechDisable", "NuiSpeechCreateGrammar",
    "NuiSpeechLoadGrammar", "NuiSpeechUnloadGrammar",
    "NuiSpeechCommitGrammar", "NuiSpeechStartRecognition",
    "NuiSpeechStopRecognition", "NuiSpeechSetEventInterest",
    "NuiSpeechSetGrammarState", "NuiSpeechSetRuleState",
    "NuiSpeechCreateRule", "NuiSpeechCreateState",
    "NuiSpeechAddWordTransition", "NuiSpeechGetEvents",
    "NuiSpeechDestroyEvent", "NuiMetaCpuEvent",
]

LINE = re.compile(r"^/\* ([0-9A-F]{8}) [0-9A-F]{8}  ((?:[0-9A-F]{2} ){4})\*/\t(.*)$")
FN = re.compile(r'^\.fn ("?)(.+?)\1, (global|local|weak)')


def parse(asm_dir):
    """name -> list of (addr, word, insn_text); also addr -> name."""
    funcs = {}
    by_addr = {}
    for root, _, files in os.walk(asm_dir):
        for f in files:
            if not f.endswith(".s"):
                continue
            cur = None
            with open(os.path.join(root, f), errors="replace") as fh:
                for line in fh:
                    m = FN.match(line)
                    if m:
                        cur = m.group(2)
                        funcs.setdefault(cur, [])
                        continue
                    if line.startswith(".endfn"):
                        cur = None
                        continue
                    if cur is None:
                        continue
                    m = LINE.match(line)
                    if not m:
                        continue
                    addr = int(m.group(1), 16)
                    word = int(m.group(2).replace(" ", ""), 16)
                    if not funcs[cur]:
                        by_addr[addr] = cur
                    funcs[cur].append((addr, word, m.group(3)))
    return funcs, by_addr


def mask_of(word, text):
    op = word >> 26
    if "@ha" in text or "@l" in text or "@h" in text:
        return 0xFFFF0000
    parts = text.split(None, 1)
    mnem = parts[0] if parts else ""
    operands = parts[1] if len(parts) > 1 else ""
    if op == 18:  # b/bl/ba/bla
        tgt = operands.strip()
        if not tgt.startswith(".L_"):
            return 0xFC000003
    if op == 16:  # bc
        if ".L_" not in operands:
            return 0xFFFF0003
    return 0xFFFFFFFF


def masked(fn_insns, n):
    out = []
    for addr, word, text in fn_insns[:n]:
        out.append((word, mask_of(word, text)))
    return out


def tail_branch(fn_insns, by_addr, funcs):
    """Index and target name of the last unconditional branch to a symbol."""
    for i in range(min(len(fn_insns), MAX_WORDS) - 1, -1, -1):
        addr, word, text = fn_insns[i]
        if (word >> 26) == 18 and not text.split(None, 1)[1].startswith(".L_"):
            li = word & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            tgt = (addr + li) & 0xFFFFFFFF
            name = by_addr.get(tgt)
            if name:
                return i, name
    return None


def matches(sig, insns):
    if len(insns) < len(sig):
        return False
    for (w, m), (_, word, _) in zip(sig, insns):
        if (word & m) != (w & m):
            return False
    return True


def main():
    asm_dir, version, out = sys.argv[1:4]
    funcs, by_addr = parse(asm_dir)
    starts = [f for f in funcs.values() if f]
    entries = []
    for name in API:
        insns = funcs.get(name)
        if not insns:
            sys.exit(f"{name}: not in the listing")
        n = min(len(insns), MAX_WORDS)
        sig = masked(insns, n)
        dup = [f for f in starts if f is not insns and matches(sig, f)]
        follow = None
        short_thunk = len(sig) < 8 and tail_branch(insns, by_addr, funcs)
        if dup or short_thunk:
            tb = tail_branch(insns, by_addr, funcs)
            if not tb:
                sys.exit(f"{name}: ambiguous ({len(dup)}) and has no tail branch")
            idx, tname = tb
            tinsns = funcs[tname]
            tsig = masked(tinsns, min(len(tinsns), TARGET_WORDS))
            still = []
            for f in dup:
                t2 = tail_branch(f, by_addr, funcs)
                if t2 and t2[0] == idx and matches(tsig, funcs[t2[1]]):
                    still.append(f)
            if still:
                sys.exit(f"{name}: ambiguous even with the XREF anchor "
                         f"({[s[0][0] for s in still]})")
            follow = (idx, tname, tsig)
        entries.append((name, sig, follow, len(dup)))
        print(f"{name:48} words={len(sig):2} "
              f"{'anchor->' + follow[1] if follow else 'unique'}"
              f"{' (' + str(len(dup)) + ' body twins)' if dup else ''}",
              file=sys.stderr)

    ident = version.replace(".", "_")
    with open(out, "w") as o:
        o.write("// GENERATED by tools/nui/gen_signatures.py -- do not edit.\n")
        o.write(f"// NUI SDK {version}: masked word signatures of the public "
                "API, from a dtk split\n// listing of an image that statically "
                "links this version. No addresses.\n\n")
        for name, sig, follow, _ in entries:
            o.write(f"static const NuiSigWord k{ident}_{name}[] = {{")
            o.write(", ".join(f"{{0x{w & m:08X}u, 0x{m:08X}u}}" for w, m in sig))
            o.write("};\n")
            if follow:
                o.write(f"static const NuiSigWord k{ident}_{name}_target[] = {{")
                o.write(", ".join(f"{{0x{w & m:08X}u, 0x{m:08X}u}}"
                                  for w, m in follow[2]))
                o.write("};\n")
        o.write(f"\nstatic const NuiSdkSignature kNuiSdk_{ident}[] = {{\n")
        for name, sig, follow, _ in entries:
            if follow:
                o.write(f"    {{\"{name}\", k{ident}_{name}, {len(sig)}, "
                        f"{follow[0]}, k{ident}_{name}_target, "
                        f"{len(follow[2])}, \"{follow[1]}\"}},\n")
            else:
                o.write(f"    {{\"{name}\", k{ident}_{name}, {len(sig)}, -1, "
                        "nullptr, 0, nullptr},\n")
        o.write("};\n")


if __name__ == "__main__":
    main()
