#!/usr/bin/env python3
"""
dc3_extract_addresses.py - Extract patched symbol addresses from dc3-decomp MAP file.

Parses an MSVC-style MAP file (default.map) produced by the dc3-decomp build and
outputs a C++ header with constexpr addresses for all symbols patched by
emulator.cc's DC3 workaround code.

Usage:
    python3 tools/dc3_extract_addresses.py [path-to-map-file] [-o output-file]

    Default MAP path: ~/code/milohax/dc3-decomp/build/373307D9/default.map
    Default output:   stdout

The generated header can be included by emulator.cc to keep addresses in sync
with the decomp build without manual edits.

MAP file format (MSVC linker):
    Each symbol line in the "Publics by Value" section looks like:

     SSSS:OOOOOOOO       symbol_name                AAAAAAAA [f] [i] lib:object.obj

    where SSSS = section number, OOOOOOOO = offset within section,
    AAAAAAAA = Rva+Base (the guest virtual address we need),
    f = function flag, i = imported flag.

    C++ mangled names use MSVC decoration: ?Name@Class@@... for methods,
    and plain names for C-linkage functions.
"""

import argparse
import os
import re
import sys
from collections import OrderedDict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

@dataclass
class MapSymbol:
    """A single symbol entry from the MAP file."""
    section: str          # e.g. "0006"
    offset: str           # e.g. "01271914"
    name: str             # Symbol name (may be mangled)
    address: int          # Rva+Base as integer
    lib_object: str       # e.g. "nuiruntime.obj"
    is_function: bool     # 'f' flag present
    is_imported: bool     # 'i' flag present


@dataclass
class PatchSymbol:
    """A symbol we need to look up, with metadata for the C++ output."""
    display_name: str       # Human-readable name (e.g. "NuiInitialize")
    map_name: str           # Primary name to search in the MAP file
    alt_names: list = field(default_factory=list)  # Alternative/mangled names
    expected_obj: str = ""  # Expected .obj file for disambiguation
    return_val: str = "kLiR3_0"  # C++ constant for insn0
    category: str = ""      # Grouping category
    comment: str = ""       # Optional inline comment


# ---------------------------------------------------------------------------
# Patch symbol definitions - mirrors emulator.cc decomp_patches[]
# ---------------------------------------------------------------------------

def build_patch_symbols() -> list:
    """Build the list of all symbols we need from the MAP file.

    Each entry corresponds to a row in emulator.cc's decomp_patches[] or
    other patched address constants.
    """
    symbols = []

    def add(display, map_name=None, alt_names=None, obj="",
            ret="kLiR3_0", cat="", comment=""):
        symbols.append(PatchSymbol(
            display_name=display,
            map_name=map_name or display,
            alt_names=alt_names or [],
            expected_obj=obj,
            return_val=ret,
            category=cat,
            comment=comment,
        ))

    # -- Core lifecycle (nuiruntime.obj) --
    cat = "nui_core"
    add("NuiInitialize",  obj="nuiruntime.obj", cat=cat)
    add("NuiShutdown",    obj="nuiruntime.obj", cat=cat)

    # -- Skeleton tracking (nuiskeleton.obj) --
    cat = "nui_skeleton"
    add("NuiSkeletonTrackingEnable",       obj="nuiskeleton.obj", cat=cat)
    add("NuiSkeletonTrackingDisable",      obj="nuiskeleton.obj", cat=cat)
    add("NuiSkeletonSetTrackedSkeletons",  obj="nuiskeleton.obj", cat=cat)
    add("NuiSkeletonGetNextFrame",         obj="nuiskeleton.obj", cat=cat,
        ret="kLiR3_Neg1")

    # -- Image streams (nuiimagecamera.obj) --
    cat = "nui_image"
    add("NuiImageStreamOpen",              obj="nuiimagecamera.obj", cat=cat)
    add("NuiImageStreamGetNextFrame",      obj="nuiimagecamera.obj", cat=cat,
        ret="kLiR3_Neg1")
    add("NuiImageStreamReleaseFrame",      obj="nuiimagecamera.obj", cat=cat)
    add("NuiImageGetColorPixelCoordinatesFromDepthPixel",
        obj="nuiimagecamera.obj", cat=cat)

    # -- Audio (nuiaudio.obj) --
    cat = "nui_audio"
    add("NuiAudioCreate",                      obj="nuiaudio.obj", cat=cat,
        ret="kLiR3_Neg1")
    add("NuiAudioCreatePrivate",               obj="nuiaudio.obj", cat=cat,
        ret="kLiR3_Neg1")
    add("NuiAudioRegisterCallbacks",           obj="nuiaudio.obj", cat=cat)
    add("NuiAudioUnregisterCallbacks",         obj="nuiaudio.obj", cat=cat)
    add("NuiAudioRegisterCallbacksPrivate",    obj="nuiaudio.obj", cat=cat)
    add("NuiAudioUnregisterCallbacksPrivate",  obj="nuiaudio.obj", cat=cat)
    add("NuiAudioRelease",                     obj="nuiaudio.obj", cat=cat)

    # -- Camera properties (nuiimagecameraproperties.obj / nuidetroit.obj) --
    cat = "nui_camera"
    add("NuipCameraSetProperty",
        map_name="?NuipCameraSetProperty@@YAJW4_NUI_CAMERA_TYPE@@W4_NUIP_CAMERA_OWNER@@W4_NUI_CAMERA_PROPERTY@@J@Z",
        alt_names=["NuipCameraSetProperty"],
        obj="nuiimagecameraproperties.obj", cat=cat)
    add("NuiCameraSetProperty",            obj="nuiimagecameraproperties.obj", cat=cat)
    add("NuiCameraElevationGetAngle",      obj="nuidetroit.obj", cat=cat)
    add("NuiCameraElevationSetAngle",      obj="nuidetroit.obj", cat=cat)
    add("NuiCameraAdjustTilt",             obj="nuidetroit.obj", cat=cat)
    add("NuiCameraGetNormalToGravity",     obj="nuidetroit.obj", cat=cat)
    add("NuiCameraSetExposureRegionOfInterest",
        obj="nuiimagecameraproperties.obj", cat=cat)
    add("NuiCameraGetExposureRegionOfInterest",
        obj="nuiimagecameraproperties.obj", cat=cat)
    add("NuiCameraGetProperty",            obj="nuiimagecameraproperties.obj", cat=cat)
    add("NuiCameraGetPropertyF",           obj="nuiimagecameraproperties.obj", cat=cat)

    # -- Identity (identityapi.obj) --
    cat = "nui_identity"
    add("NuiIdentityEnroll",                      obj="identityapi.obj", cat=cat)
    add("NuiIdentityIdentify",                    obj="identityapi.obj", cat=cat)
    add("NuiIdentityGetEnrollmentInformation",    obj="identityapi.obj", cat=cat)
    add("NuiIdentityAbort",                       obj="identityapi.obj", cat=cat)

    # -- Fitness (nuifitnesslib.obj / nuifitnessapi.obj) --
    cat = "nui_fitness"
    add("NuiFitnessStartTracking",         obj="nuifitnesslib.obj", cat=cat,
        ret="kLiR3_Neg1")
    add("NuiFitnessPauseTracking",         obj="nuifitnesslib.obj", cat=cat,
        ret="kLiR3_Neg1")
    add("NuiFitnessResumeTracking",        obj="nuifitnesslib.obj", cat=cat,
        ret="kLiR3_Neg1")
    add("NuiFitnessStopTracking",          obj="nuifitnesslib.obj", cat=cat,
        ret="kLiR3_Neg1")
    add("NuiFitnessGetCurrentFitnessData", obj="nuifitnessapi.obj", cat=cat,
        ret="kLiR3_Neg1")

    # -- Wave gesture (nuiwave.obj) --
    cat = "nui_wave"
    add("NuiWaveSetEnabled",                  obj="nuiwave.obj", cat=cat,
        ret="kLiR3_Neg1")
    add("NuiWaveGetGestureOwnerProgress",     obj="nuiwave.obj", cat=cat,
        ret="kLiR3_Neg1")

    # -- Head tracking (nuiheadposition.obj / headtrackingapi.obj) --
    cat = "nui_head"
    add("NuiHeadPositionDisable",       obj="nuiheadposition.obj", cat=cat)
    add("NuiHeadOrientationDisable",    obj="headtrackingapi.obj", cat=cat)

    # -- Speech (xspeechapi.obj) --
    cat = "nui_speech"
    add("NuiSpeechEnable",             obj="xspeechapi.obj", cat=cat)
    add("NuiSpeechDisable",            obj="xspeechapi.obj", cat=cat)
    add("NuiSpeechCreateGrammar",      obj="xspeechapi.obj", cat=cat)
    add("NuiSpeechLoadGrammar",        obj="xspeechapi.obj", cat=cat)
    add("NuiSpeechUnloadGrammar",      obj="xspeechapi.obj", cat=cat)
    add("NuiSpeechCommitGrammar",      obj="xspeechapi.obj", cat=cat)
    add("NuiSpeechStartRecognition",   obj="xspeechapi.obj", cat=cat)
    add("NuiSpeechStopRecognition",    obj="xspeechapi.obj", cat=cat)
    add("NuiSpeechSetEventInterest",   obj="xspeechapi.obj", cat=cat)
    add("NuiSpeechSetGrammarState",    obj="xspeechapi.obj", cat=cat)
    add("NuiSpeechSetRuleState",       obj="xspeechapi.obj", cat=cat)
    add("NuiSpeechCreateRule",         obj="xspeechapi.obj", cat=cat)
    add("NuiSpeechCreateState",        obj="xspeechapi.obj", cat=cat)
    add("NuiSpeechAddWordTransition",  obj="xspeechapi.obj", cat=cat)
    add("NuiSpeechGetEvents",          obj="xspeechapi.obj", cat=cat,
        ret="kLiR3_Neg1")
    add("NuiSpeechDestroyEvent",       obj="xspeechapi.obj", cat=cat)
    add("NuiSpeechEmulateRecognition", obj="xspeechapi.obj", cat=cat)

    # -- SmartGlass / XBC (xbcimpl.obj) --
    cat = "xbc"
    add("CXbcImpl::Initialize",
        map_name="?Initialize@CXbcImpl@@SAJP6AXJPAU_XBC_EVENT_PARAMS@@PAX@Z1@Z",
        alt_names=["Initialize@CXbcImpl"],
        obj="xbcimpl.obj", cat=cat)
    add("CXbcImpl::DoWork",
        map_name="?DoWork@CXbcImpl@@SAJXZ",
        alt_names=["DoWork@CXbcImpl"],
        obj="xbcimpl.obj", cat=cat)
    add("CXbcImpl::SendJSON",
        map_name="?SendJSON@CXbcImpl@@SAJW4_XBC_DELIVERY_METHOD@@KPAUHJSONWRITER__@@PAX@Z",
        alt_names=["SendJSON@CXbcImpl"],
        obj="xbcimpl.obj", cat=cat)

    # -- D3D NUI (nui.obj) --
    cat = "d3d_nui"
    add("D3DDevice_NuiInitialize",
        map_name="?D3DDevice_NuiInitialize@@YAJPAPAUD3DDevice@@@Z",
        alt_names=["D3DDevice_NuiInitialize"],
        obj="nui.obj", cat=cat)

    # -- Misc (st.obj) --
    cat = "misc"
    add("NuiMetaCpuEvent", obj="st.obj", cat=cat)

    return symbols


def build_extra_symbols() -> list:
    """Build the list of non-decomp-patch symbols we also need.

    These are used by other fixup code in emulator.cc (CRT tables, thread
    notify routines, etc.).
    """
    symbols = []

    def add(display, map_name=None, alt_names=None, obj="", cat="", comment=""):
        symbols.append(PatchSymbol(
            display_name=display,
            map_name=map_name or display,
            alt_names=alt_names or [],
            expected_obj=obj,
            category=cat,
            comment=comment,
        ))

    # -- CRT thread notify (xregisterthreadnotifyroutine.obj) --
    cat = "thread_notify"
    add("XapiCallThreadNotifyRoutines",
        obj="xregisterthreadnotifyroutine.obj", cat=cat)
    add("XRegisterThreadNotifyRoutine",
        obj="xregisterthreadnotifyroutine.obj", cat=cat)
    add("XapiProcessLock",
        obj="xregisterthreadnotifyroutine.obj", cat=cat,
        comment="RTL_CRITICAL_SECTION")
    add("XapiThreadNotifyRoutineList",
        obj="xregisterthreadnotifyroutine.obj", cat=cat,
        comment="LIST_ENTRY")

    # -- CRT initializer table bounds --
    cat = "crt_tables"
    add("__xc_a", obj="auto_08_82F05C00_data.obj", cat=cat,
        comment="C++ constructor table start")
    add("__xc_z", obj="auto_08_82F05C00_data.obj", cat=cat,
        comment="C++ constructor table end")
    add("__xi_a", obj="auto_08_82F05C00_data.obj", cat=cat,
        comment="C initializer table start")
    add("__xi_z", obj="auto_08_82F05C00_data.obj", cat=cat,
        comment="C initializer table end")

    # -- DC3 Globals / Probes --
    cat = "dc3_probes"
    add("TheDebug", map_name="?TheDebug@@3VDebug@@A", obj="Debug.obj", cat=cat)
    add("Debug::Fail", map_name="?Fail@Debug@@QAAXPBDPAX@Z", obj="Debug.obj", cat=cat)
    add("gSystemConfig", map_name="?gSystemConfig@@3PAVDataArray@@A", obj="System.obj", cat=cat)
    add("MemInit", map_name="?MemInit@@YAXXZ", obj="MemMgr.obj", cat=cat)
    add("MemAlloc", map_name="?MemAlloc@@YAPAXHPBDH0H@Z", obj="MemMgr.obj", cat=cat)
    return symbols


# ---------------------------------------------------------------------------
# MAP file parser
# ---------------------------------------------------------------------------

# Regex for the "Publics by Value" section lines.
# Matches lines like:
#  0006:01271914       NuiInitialize              83681914 f i nuiruntime.obj
#  000a:00055f20       __xc_a                     83b0bb20     auto_08_82F05C00_data.obj
#
# Groups: section, offset, symbol_name, rva_base, flags_and_obj
_SYMBOL_RE = re.compile(
    r'^\s*'
    r'([0-9a-fA-F]{4}):([0-9a-fA-F]{8})'   # section:offset
    r'\s+'
    r'(\S+)'                                  # symbol name
    r'\s+'
    r'([0-9a-fA-F]{8})'                      # Rva+Base address
    r'(.*?)'                                  # flags + lib:object (rest of line)
    r'\s*$'
)


def parse_map_file(path: str) -> dict:
    """Parse MAP file and return dict of symbol_name -> list[MapSymbol].

    A symbol name can appear multiple times (e.g. in different sections),
    so we store a list per name.
    """
    symbols = {}
    in_publics = False

    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        for line in f:
            # Detect start of Publics section
            if 'Publics by Value' in line:
                in_publics = True
                continue

            # Detect end of Publics section (blank lines between sections
            # are fine; we stop at known section headers)
            if in_publics and line.strip().startswith('entry point at'):
                break
            if in_publics and line.strip().startswith('Static symbols'):
                # Some MAP files have a "Static symbols" section after publics
                break

            if not in_publics:
                continue

            m = _SYMBOL_RE.match(line)
            if not m:
                continue

            section = m.group(1)
            offset = m.group(2)
            name = m.group(3)
            rva_base = int(m.group(4), 16)
            tail = m.group(5).strip()

            # Parse flags and lib:object from tail
            # tail might look like "f i xspeechapi.obj" or "nuiruntime.obj"
            # or "f   nuiaudio.obj" or "<absolute>"
            parts = tail.split()
            is_function = 'f' in parts
            is_imported = 'i' in parts

            # The last token that ends in .obj is the object file
            lib_obj = ""
            for p in reversed(parts):
                if p.endswith('.obj'):
                    lib_obj = p
                    break

            sym = MapSymbol(
                section=section,
                offset=offset,
                name=name,
                address=rva_base,
                lib_object=lib_obj,
                is_function=is_function,
                is_imported=is_imported,
            )

            if name not in symbols:
                symbols[name] = []
            symbols[name].append(sym)

    return symbols


# ---------------------------------------------------------------------------
# Symbol lookup
# ---------------------------------------------------------------------------

def lookup_symbol(
    patch: PatchSymbol,
    symbol_db: dict,
) -> Optional[MapSymbol]:
    """Look up a PatchSymbol in the MAP symbol database.

    Tries the primary map_name first, then alt_names. If multiple matches
    exist, prefers the one from expected_obj. Returns None if not found.
    """
    names_to_try = [patch.map_name] + patch.alt_names

    candidates = []
    for name in names_to_try:
        if name in symbol_db:
            candidates.extend(symbol_db[name])

    if not candidates:
        # Try partial/substring matching for mangled names containing
        # the display_name as a component (e.g. "Initialize@CXbcImpl" in
        # "?Initialize@CXbcImpl@@SAJP6A...")
        for alt in patch.alt_names:
            for sym_name, sym_list in symbol_db.items():
                if alt in sym_name:
                    candidates.extend(sym_list)

    if not candidates:
        return None

    # Deduplicate by address
    seen = set()
    unique = []
    for c in candidates:
        if c.address not in seen:
            seen.add(c.address)
            unique.append(c)
    candidates = unique

    if len(candidates) == 1:
        return candidates[0]

    # Prefer match from expected .obj file
    if patch.expected_obj:
        obj_matches = [c for c in candidates if c.lib_object == patch.expected_obj]
        if len(obj_matches) == 1:
            return obj_matches[0]
        if obj_matches:
            # Multiple from same obj; prefer function symbols
            func_matches = [c for c in obj_matches if c.is_function]
            if func_matches:
                return func_matches[0]
            return obj_matches[0]

    # Prefer function symbols in code sections (0006 = .text)
    code_matches = [c for c in candidates if c.section == '0006']
    if code_matches:
        return code_matches[0]

    return candidates[0]


# ---------------------------------------------------------------------------
# Output generation
# ---------------------------------------------------------------------------

CATEGORY_NAMES = OrderedDict([
    ("nui_core",     "Core lifecycle (nuiruntime.obj)"),
    ("nui_skeleton", "Skeleton tracking (nuiskeleton.obj)"),
    ("nui_image",    "Image streams (nuiimagecamera.obj)"),
    ("nui_audio",    "Audio (nuiaudio.obj)"),
    ("nui_camera",   "Camera properties (nuiimagecameraproperties.obj, nuidetroit.obj)"),
    ("nui_identity", "Identity (identityapi.obj)"),
    ("nui_fitness",  "Fitness (nuifitnesslib.obj, nuifitnessapi.obj)"),
    ("nui_wave",     "Wave gesture (nuiwave.obj)"),
    ("nui_head",     "Head tracking (nuiheadposition.obj, headtrackingapi.obj)"),
    ("nui_speech",   "Speech (xspeechapi.obj)"),
    ("xbc",          "SmartGlass / XBC (xbcimpl.obj)"),
    ("d3d_nui",      "D3D NUI (nui.obj)"),
    ("misc",         "Misc"),
    ("thread_notify","CRT thread notify routines"),
    ("crt_tables",   "CRT initializer table bounds"),
    ("dc3_probes",    "DC3 Globals / Probes"),
])


def generate_header(
    patch_results: list,
    extra_results: list,
    map_path: str,
    timestamp: str,
) -> str:
    """Generate a C++ header with all resolved addresses."""

    lines = []
    lines.append("// Auto-generated by tools/dc3_extract_addresses.py")
    lines.append(f"// MAP file: {map_path}")
    lines.append(f"// Generated: {timestamp}")
    lines.append("//")
    lines.append("// This file contains guest virtual addresses extracted from the")
    lines.append("// dc3-decomp MAP file for use by emulator.cc DC3 workarounds.")
    lines.append("// Regenerate after every dc3-decomp rebuild.")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("namespace xe {")
    lines.append("namespace dc3_addrs {")
    lines.append("")

    # Group patch symbols by category
    cat_groups = OrderedDict()
    for patch, sym in patch_results:
        cat = patch.category
        if cat not in cat_groups:
            cat_groups[cat] = []
        cat_groups[cat].append((patch, sym))

    # Also group extra symbols
    extra_groups = OrderedDict()
    for patch, sym in extra_results:
        cat = patch.category
        if cat not in extra_groups:
            extra_groups[cat] = []
        extra_groups[cat].append((patch, sym))

    # Emit decomp_patches[] initializer data
    lines.append("// ---- decomp_patches[] entries ----")
    lines.append("// Use these to populate the NuiPatch decomp_patches[] array.")
    lines.append("// Format: {address, insn0, insn1, name}")
    lines.append("")

    for cat_key in CATEGORY_NAMES:
        if cat_key not in cat_groups:
            continue
        group = cat_groups[cat_key]
        cat_label = CATEGORY_NAMES.get(cat_key, cat_key)

        lines.append(f"// {cat_label}")
        for patch, sym in group:
            if sym is None:
                lines.append(
                    f"// WARNING: {patch.display_name} - NOT FOUND IN MAP")
                continue

            # Sanitize display name for C++ identifier
            ident = patch.display_name.replace("::", "_").replace(" ", "_")
            addr_hex = f"0x{sym.address:08X}"
            comment_parts = [f"// {sym.lib_object}"]
            if patch.comment:
                comment_parts.append(patch.comment)

            lines.append(
                f"constexpr uint32_t k{ident} = {addr_hex};  "
                f"{' '.join(comment_parts)}")
        lines.append("")

    # Emit the extra symbols (thread notify, CRT tables)
    lines.append("// ---- Other patched addresses ----")
    lines.append("")

    for cat_key in CATEGORY_NAMES:
        if cat_key not in extra_groups:
            continue
        group = extra_groups[cat_key]
        cat_label = CATEGORY_NAMES.get(cat_key, cat_key)

        lines.append(f"// {cat_label}")
        for patch, sym in group:
            if sym is None:
                lines.append(
                    f"// WARNING: {patch.display_name} - NOT FOUND IN MAP")
                continue

            ident = patch.display_name.replace("::", "_").replace(" ", "_")
            addr_hex = f"0x{sym.address:08X}"
            comment_parts = [f"// {sym.lib_object}"]
            if patch.comment:
                comment_parts.append(f"({patch.comment})")

            lines.append(
                f"constexpr uint32_t k{ident} = {addr_hex};  "
                f"{' '.join(comment_parts)}")
        lines.append("")

    # Emit a ready-to-paste decomp_patches[] array
    lines.append("// ---- Ready-to-paste decomp_patches[] array ----")
    lines.append("// Copy this block into emulator.cc to replace the")
    lines.append("// decomp_patches[] initializer.")
    lines.append("//")

    for cat_key in CATEGORY_NAMES:
        if cat_key not in cat_groups:
            continue
        group = cat_groups[cat_key]
        cat_label = CATEGORY_NAMES.get(cat_key, cat_key)

        lines.append(f"//     // {cat_label}")
        for patch, sym in group:
            if sym is None:
                lines.append(
                    f"//     // WARNING: {patch.display_name} NOT FOUND")
                continue

            addr_hex = f"0x{sym.address:08X}"
            ret = patch.return_val
            name_str = patch.display_name

            # Format the name string - quote it
            lines.append(
                f'//     {{{addr_hex}, {ret}, kBlr, "{name_str}"}},')
        lines.append("//")

    # Emit the kNotifyFuncs[] array
    lines.append("")
    lines.append("// ---- kNotifyFuncs[] (thread notify stubs) ----")
    notify_syms = [
        (p, s) for p, s in extra_results
        if p.display_name in (
            "XapiCallThreadNotifyRoutines",
            "XRegisterThreadNotifyRoutine",
        )
    ]
    addrs = []
    for p, s in notify_syms:
        if s:
            addrs.append(f"0x{s.address:08X}")
        else:
            addrs.append(f"0x00000000 /* {p.display_name} NOT FOUND */")
    lines.append(f"//     const uint32_t kNotifyFuncs[] = {{{', '.join(addrs)}}};")

    # Emit CRT table bounds
    lines.append("")
    lines.append("// ---- CRT table bounds ----")
    crt_syms = {
        p.display_name: s for p, s in extra_results
        if p.category == "crt_tables"
    }
    xc_a = crt_syms.get("__xc_a")
    xc_z = crt_syms.get("__xc_z")
    xi_a = crt_syms.get("__xi_a")
    xi_z = crt_syms.get("__xi_z")

    def addr_or_missing(sym, name):
        if sym:
            return f"0x{sym.address:08X}"
        return f"0x00000000 /* {name} NOT FOUND */"

    lines.append("//     CrtTable tables[] = {")
    lines.append(
        f'//         {{{addr_or_missing(xc_a, "__xc_a")}, '
        f'{addr_or_missing(xc_z, "__xc_z")}, '
        f'"__xc_a..__xc_z (C++ constructors)"}},')
    lines.append(
        f'//         {{{addr_or_missing(xi_a, "__xi_a")}, '
        f'{addr_or_missing(xi_z, "__xi_z")}, '
        f'"__xi_a..__xi_z (C initializers)"}},')
    lines.append("//     };")

    lines.append("")
    lines.append("}  // namespace dc3_addrs")
    lines.append("}  // namespace xe")
    lines.append("")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Verification / diff reporting
# ---------------------------------------------------------------------------

def print_verification(patch_results, extra_results, file=sys.stderr):
    """Print a summary of found/missing symbols for quick verification."""
    total = len(patch_results) + len(extra_results)
    found = sum(1 for _, s in patch_results if s is not None)
    found += sum(1 for _, s in extra_results if s is not None)
    missing = total - found

    print(f"\n{'='*60}", file=file)
    print(f"  Symbol resolution summary", file=file)
    print(f"  Total: {total}  Found: {found}  Missing: {missing}", file=file)
    print(f"{'='*60}", file=file)

    if missing > 0:
        print(f"\n  MISSING SYMBOLS:", file=file)
        for patch, sym in patch_results + extra_results:
            if sym is None:
                print(f"    - {patch.display_name} "
                      f"(expected in {patch.expected_obj})", file=file)

    # Print a compact address table for quick visual diff
    print(f"\n  Resolved addresses:", file=file)
    for patch, sym in patch_results + extra_results:
        if sym is not None:
            status = ""
            if sym.lib_object and patch.expected_obj:
                if sym.lib_object != patch.expected_obj:
                    status = f"  (from {sym.lib_object}, expected {patch.expected_obj})"
            print(f"    0x{sym.address:08X}  {patch.display_name}{status}",
                  file=file)
        else:
            print(f"    ----------  {patch.display_name}  ** NOT FOUND **",
                  file=file)
    print(file=file)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    default_map = os.path.expanduser(
        "~/code/milohax/dc3-decomp/build/373307D9/default.map"
    )

    parser = argparse.ArgumentParser(
        description="Extract DC3 decomp patch addresses from MAP file.",
        epilog="Reads an MSVC-style MAP file and outputs a C++ header with "
               "constexpr addresses for all symbols patched by emulator.cc.",
    )
    parser.add_argument(
        "map_file",
        nargs="?",
        default=default_map,
        help=f"Path to default.map (default: {default_map})",
    )
    parser.add_argument(
        "-o", "--output",
        default=None,
        help="Output file path (default: stdout)",
    )
    parser.add_argument(
        "-q", "--quiet",
        action="store_true",
        help="Suppress verification summary on stderr",
    )
    args = parser.parse_args()

    map_path = os.path.expanduser(args.map_file)
    if not os.path.isfile(map_path):
        print(f"Error: MAP file not found: {map_path}", file=sys.stderr)
        sys.exit(1)

    # Parse MAP file
    print(f"Parsing MAP file: {map_path}", file=sys.stderr)
    symbol_db = parse_map_file(map_path)
    print(f"  Loaded {sum(len(v) for v in symbol_db.values())} symbol entries "
          f"({len(symbol_db)} unique names)", file=sys.stderr)

    # Build lookup lists
    patch_symbols = build_patch_symbols()
    extra_symbols = build_extra_symbols()

    # Resolve all symbols
    patch_results = []
    for ps in patch_symbols:
        sym = lookup_symbol(ps, symbol_db)
        if sym is None:
            print(f"  WARNING: {ps.display_name} not found in MAP",
                  file=sys.stderr)
        patch_results.append((ps, sym))

    extra_results = []
    for ps in extra_symbols:
        sym = lookup_symbol(ps, symbol_db)
        if sym is None:
            print(f"  WARNING: {ps.display_name} not found in MAP",
                  file=sys.stderr)
        extra_results.append((ps, sym))

    # Verification summary
    if not args.quiet:
        print_verification(patch_results, extra_results)

    # Generate output
    from datetime import datetime, timezone
    timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

    header = generate_header(patch_results, extra_results, map_path, timestamp)

    if args.output:
        out_path = os.path.expanduser(args.output)
        os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
        with open(out_path, 'w') as f:
            f.write(header)
        print(f"Output written to: {out_path}", file=sys.stderr)
    else:
        print(header)

    # Return non-zero if any symbols were missing
    missing = sum(1 for _, s in patch_results + extra_results if s is None)
    if missing > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
