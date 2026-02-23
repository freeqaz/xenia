#!/usr/bin/env python3
"""
Detailed PE/COFF analysis of PoolAlloc.obj
Little-endian on disk (Xbox 360 / PowerPC target).
"""

import struct
import sys

OBJ_PATH = "/home/free/code/milohax/dc3-decomp/build/373307D9/obj/system/utl/PoolAlloc.obj"

# --- Constants ---
STORAGE_CLASS_NAMES = {
    0: "NULL",
    1: "AUTOMATIC",
    2: "EXTERNAL",
    3: "STATIC",
    4: "REGISTER",
    5: "EXTERN_DEF",
    6: "LABEL",
    7: "UNDEF_LABEL",
    8: "MEMBER_OF_STRUCT",
    9: "ARGUMENT",
    10: "STRUCT_TAG",
    11: "MEMBER_OF_UNION",
    12: "UNION_TAG",
    13: "TYPE_DEFINITION",
    14: "UNDEF_STATIC",
    15: "ENUM_TAG",
    16: "MEMBER_OF_ENUM",
    17: "REGISTER_PARAM",
    18: "BIT_FIELD",
    100: "BLOCK",
    101: "FUNCTION",
    102: "END_OF_STRUCT",
    103: "FILE",
    104: "SECTION",
    105: "WEAK_EXTERNAL",
    107: "CLR_TOKEN",
}

COMDAT_SELECTION = {
    0: "NONE(0)",
    1: "NODUPLICATES",
    2: "ANY",
    3: "SAME_SIZE",
    4: "EXACT_MATCH",
    5: "ASSOCIATIVE",
    6: "LARGEST",
}

SECTION_FLAG_BITS = {
    0x00000008: "NO_PAD",
    0x00000020: "CNT_CODE",
    0x00000040: "CNT_INITIALIZED_DATA",
    0x00000080: "CNT_UNINITIALIZED_DATA",
    0x00000100: "LNK_OTHER",
    0x00000200: "LNK_INFO",
    0x00000800: "LNK_REMOVE",
    0x00001000: "LNK_COMDAT",
    0x00004000: "NO_DEFER_SPEC_EXC",
    0x00008000: "GPREL",
    0x00100000: "ALIGN_1BYTES",
    0x00200000: "ALIGN_2BYTES",
    0x00300000: "ALIGN_4BYTES",
    0x00400000: "ALIGN_8BYTES",
    0x00500000: "ALIGN_16BYTES",
    0x00600000: "ALIGN_32BYTES",
    0x00700000: "ALIGN_64BYTES",
    0x00800000: "ALIGN_128BYTES",
    0x00900000: "ALIGN_256BYTES",
    0x00A00000: "ALIGN_512BYTES",
    0x00B00000: "ALIGN_1024BYTES",
    0x00C00000: "ALIGN_2048BYTES",
    0x00D00000: "ALIGN_4096BYTES",
    0x00E00000: "ALIGN_8192BYTES",
    0x01000000: "LNK_NRELOC_OVFL",
    0x02000000: "MEM_DISCARDABLE",
    0x04000000: "MEM_NOT_CACHED",
    0x08000000: "MEM_NOT_PAGED",
    0x10000000: "MEM_SHARED",
    0x20000000: "MEM_EXECUTE",
    0x40000000: "MEM_READ",
    0x80000000: "MEM_WRITE",
}

RELOC_TYPE_NAMES_PPC = {
    0x0000: "ABSOLUTE",
    0x0001: "ADDR64",
    0x0002: "ADDR32",
    0x0003: "ADDR24",
    0x0004: "ADDR16",
    0x0005: "ADDR14",
    0x0006: "REL24",
    0x0007: "REL14",
    0x0008: "TOCREL16",
    0x0009: "TOCREL14",
    0x000A: "ADDR32NB",
    0x000B: "SECREL",
    0x000C: "SECTION",
    0x000D: "IFGLUE",
    0x000E: "IMGLUE",
    0x000F: "SECREL16",
    0x0010: "REFHI",
    0x0011: "REFLO",
    0x0012: "PAIR",
    0x0013: "SECRELLO",
    0x0014: "SECRELHI",
    0x0015: "GPREL",
    0x0016: "TOKEN",
}


def decode_section_flags(flags):
    parts = []
    align = flags & 0x00F00000
    remaining = flags & ~0x00F00000
    for bit, name in SECTION_FLAG_BITS.items():
        if bit & 0x00F00000:
            continue
        if remaining & bit:
            parts.append(name)
    if align:
        align_name = SECTION_FLAG_BITS.get(align, f"ALIGN_UNK(0x{align:08X})")
        parts.append(align_name)
    return " | ".join(parts) if parts else "NONE"


def main():
    with open(OBJ_PATH, "rb") as f:
        data = f.read()

    print(f"File size: {len(data)} bytes")
    print("=" * 100)

    # --- COFF Header (20 bytes) ---
    machine, num_sections, timestamp, symtab_offset, num_symbols, opt_hdr_size, characteristics = \
        struct.unpack_from("<HHIIIHH", data, 0)

    print(f"COFF HEADER:")
    print(f"  Machine:           0x{machine:04X}")
    print(f"  Num sections:      {num_sections}")
    print(f"  Timestamp:         0x{timestamp:08X}")
    print(f"  Symbol table off:  0x{symtab_offset:08X} ({symtab_offset})")
    print(f"  Num symbols:       {num_symbols}")
    print(f"  Opt header size:   {opt_hdr_size}")
    print(f"  Characteristics:   0x{characteristics:04X}")
    print()

    # --- String Table ---
    strtab_offset = symtab_offset + num_symbols * 18
    strtab_size = struct.unpack_from("<I", data, strtab_offset)[0]
    strtab_data = data[strtab_offset:strtab_offset + strtab_size]
    print(f"STRING TABLE at offset 0x{strtab_offset:08X}, size={strtab_size} bytes")
    print()

    def get_string(offset_in_strtab):
        end = strtab_data.index(b'\x00', offset_in_strtab)
        return strtab_data[offset_in_strtab:end].decode('utf-8', errors='replace')

    def get_symbol_name(name_bytes):
        if name_bytes[:4] == b'\x00\x00\x00\x00':
            str_offset = struct.unpack_from("<I", name_bytes, 4)[0]
            return get_string(str_offset)
        else:
            return name_bytes.split(b'\x00')[0].decode('utf-8', errors='replace')

    # --- Section Headers (40 bytes each) ---
    section_hdrs = []
    sections_offset = 20 + opt_hdr_size
    print("=" * 100)
    print("SECTIONS:")
    print("=" * 100)
    for i in range(num_sections):
        off = sections_offset + i * 40
        name_bytes = data[off:off+8]
        if name_bytes[0:1] == b'/':
            str_off = int(name_bytes[1:].split(b'\x00')[0].decode())
            sec_name = get_string(str_off)
        else:
            sec_name = name_bytes.split(b'\x00')[0].decode('utf-8', errors='replace')

        vsize, vaddr, raw_size, raw_ptr, reloc_ptr, linenum_ptr, num_relocs, num_linenums, chars = \
            struct.unpack_from("<IIIIIIHHI", data, off + 8)

        section_hdrs.append({
            'index': i + 1,
            'name': sec_name,
            'vsize': vsize,
            'vaddr': vaddr,
            'raw_size': raw_size,
            'raw_ptr': raw_ptr,
            'reloc_ptr': reloc_ptr,
            'linenum_ptr': linenum_ptr,
            'num_relocs': num_relocs,
            'num_linenums': num_linenums,
            'chars': chars,
        })

        is_comdat = "COMDAT" if chars & 0x1000 else ""
        is_code = "CODE" if chars & 0x20 else ""
        is_exec = "EXEC" if chars & 0x20000000 else ""
        is_data = "IDATA" if chars & 0x40 else ""
        is_read = "READ" if chars & 0x40000000 else ""
        is_write = "WRITE" if chars & 0x80000000 else ""

        tags = " ".join(t for t in [is_comdat, is_code, is_exec, is_data, is_read, is_write] if t)

        print(f"  Section {i+1:3d}: {sec_name:<40s} size=0x{raw_size:06X} ({raw_size:6d})  "
              f"relocs={num_relocs:3d}  chars=0x{chars:08X}  [{tags}]")
        print(f"             raw_ptr=0x{raw_ptr:06X}  reloc_ptr=0x{reloc_ptr:06X}  "
              f"flags: {decode_section_flags(chars)}")
        print()

    # --- Symbol Table ---
    print("=" * 100)
    print("SYMBOL TABLE:")
    print("=" * 100)

    symbols = []
    i = 0
    while i < num_symbols:
        off = symtab_offset + i * 18
        name_bytes = data[off:off+8]
        sym_name = get_symbol_name(name_bytes)
        value, sec_num, sym_type, storage_class, num_aux = struct.unpack_from("<IhHBB", data, off + 8)

        sc_name = STORAGE_CLASS_NAMES.get(storage_class, f"UNKNOWN({storage_class})")

        symbols.append({
            'index': i,
            'name': sym_name,
            'value': value,
            'section': sec_num,
            'type': sym_type,
            'storage_class': storage_class,
            'sc_name': sc_name,
            'num_aux': num_aux,
        })

        sec_str = f"sect={sec_num}" if sec_num > 0 else ("UNDEF" if sec_num == 0 else f"ABS({sec_num})")

        print(f"  [{i:4d}] {sym_name:<60s} val=0x{value:08X}  {sec_str:<12s}  "
              f"type=0x{sym_type:04X}  class={sc_name:<12s}  aux={num_aux}")

        # Parse aux records
        for a in range(num_aux):
            aux_off = symtab_offset + (i + 1 + a) * 18
            aux_data = data[aux_off:aux_off+18]

            if storage_class == 103:  # FILE
                fname = aux_data.rstrip(b'\x00').decode('utf-8', errors='replace')
                print(f"         AUX[{a}] FILE: {fname}")
            elif storage_class == 3 and value == 0 and sec_num > 0:
                # COMDAT section definition aux
                length, num_relocs_aux, num_linenums_aux, checksum, number, selection = \
                    struct.unpack_from("<IHHIHB", aux_data, 0)
                sel_name = COMDAT_SELECTION.get(selection, f"UNKNOWN({selection})")
                print(f"         AUX[{a}] SECTION DEF: length={length}  relocs={num_relocs_aux}  "
                      f"linenums={num_linenums_aux}  checksum=0x{checksum:08X}  "
                      f"assoc_sect={number}  selection={sel_name}")
            elif storage_class == 3 and sec_num > 0:
                length, num_relocs_aux, num_linenums_aux, checksum, number, selection = \
                    struct.unpack_from("<IHHIHB", aux_data, 0)
                print(f"         AUX[{a}] (static, val!=0): length={length}  relocs={num_relocs_aux}  "
                      f"linenums={num_linenums_aux}  checksum=0x{checksum:08X}  "
                      f"assoc_sect={number}  selection={selection}")
            else:
                print(f"         AUX[{a}] RAW: {aux_data.hex()}")

        i += 1 + num_aux

    # --- Build mappings ---
    print()
    print("=" * 100)
    print("COMDAT SECTION <-> SYMBOL MAPPING:")
    print("=" * 100)

    section_sym_map = {}
    for s in symbols:
        if s['storage_class'] == 3 and s['value'] == 0 and s['section'] > 0:
            section_sym_map[s['section']] = s

    section_externals = {}
    for s in symbols:
        if s['storage_class'] == 2 and s['section'] > 0:
            if s['section'] not in section_externals:
                section_externals[s['section']] = []
            section_externals[s['section']].append(s)

    for sec in section_hdrs:
        if not (sec['chars'] & 0x1000):
            continue
        sec_num = sec['index']
        sec_sym = section_sym_map.get(sec_num)
        ext_syms = section_externals.get(sec_num, [])

        print(f"\n  Section {sec_num}: {sec['name']}")
        if sec_sym:
            print(f"    Section symbol: [{sec_sym['index']}] {sec_sym['name']}")
        print(f"    EXTERNAL symbols:")
        for es in ext_syms:
            print(f"      [{es['index']}] {es['name']}  val=0x{es['value']:08X}")
        if not ext_syms:
            print(f"      (none)")

    # --- Check .text$x / .pdata / .xdata patterns ---
    print()
    print("=" * 100)
    print("UNWIND (.text$x, .pdata, .xdata) SECTION ANALYSIS:")
    print("=" * 100)

    for sec in section_hdrs:
        name = sec['name']
        if '.text$x' in name or '.pdata' in name or '.xdata' in name:
            sec_num = sec['index']
            sec_sym = section_sym_map.get(sec_num)
            ext_syms = section_externals.get(sec_num, [])

            print(f"\n  Section {sec_num}: {name}  (size={sec['raw_size']}, relocs={sec['num_relocs']})")
            if sec_sym:
                aux_idx = sec_sym['index'] + 1
                if sec_sym['num_aux'] > 0:
                    aux_off = symtab_offset + aux_idx * 18
                    aux_data_bytes = data[aux_off:aux_off+18]
                    length, nr, nl, ck, assoc_num, sel = struct.unpack_from("<IHHIHB", aux_data_bytes, 0)
                    sel_name = COMDAT_SELECTION.get(sel, f"UNKNOWN({sel})")
                    assoc_sec_name = section_hdrs[assoc_num-1]['name'] if 0 < assoc_num <= len(section_hdrs) else "?"
                    print(f"    Section symbol: [{sec_sym['index']}] {sec_sym['name']}")
                    print(f"    COMDAT selection={sel_name}, assoc_section={assoc_num} ({assoc_sec_name})")

    # --- RELOCATIONS for specific sections ---
    print()
    print("=" * 100)
    print("RELOCATIONS FOR SECTIONS OF INTEREST:")
    print("=" * 100)

    detailed_sections = set()
    for sec in section_hdrs:
        if sec['chars'] & 0x1000:
            detailed_sections.add(sec['index'])

    sym_by_index = {}
    for s in symbols:
        sym_by_index[s['index']] = s

    for sec in section_hdrs:
        sec_num = sec['index']
        if sec['num_relocs'] == 0:
            continue

        print(f"\n  Section {sec_num}: {sec['name']}  ({sec['num_relocs']} relocations)")
        for r in range(sec['num_relocs']):
            roff = sec['reloc_ptr'] + r * 10
            vaddr, sym_idx, rtype = struct.unpack_from("<IIH", data, roff)
            target_sym = sym_by_index.get(sym_idx, {'name': f'???[{sym_idx}]', 'section': -99})
            rtype_name = RELOC_TYPE_NAMES_PPC.get(rtype, f"0x{rtype:04X}")
            print(f"    reloc[{r:2d}]: vaddr=0x{vaddr:08X}  sym=[{sym_idx:4d}] {target_sym['name']:<50s}  "
                  f"type={rtype_name}")

    # --- Summary ---
    print()
    print("=" * 100)
    print("COMPLETE COMDAT .text SECTION -> EXTERNAL FUNCTION MAP:")
    print("=" * 100)

    for sec in section_hdrs:
        if not (sec['chars'] & 0x1000):
            continue
        if not sec['name'].startswith('.text'):
            continue
        if '.text$x' in sec['name']:
            continue

        sec_num = sec['index']
        ext_syms = section_externals.get(sec_num, [])
        sec_sym = section_sym_map.get(sec_num)

        ext_names = ", ".join(e['name'] for e in ext_syms) if ext_syms else "(none)"
        sec_sym_name = sec_sym['name'] if sec_sym else "(none)"

        print(f"  Sect {sec_num:3d}: {sec['name']:<35s}  size={sec['raw_size']:5d}  "
              f"relocs={sec['num_relocs']:3d}  "
              f"sec_sym={sec_sym_name:<50s}  externals={ext_names}")

    # --- Check for "missing" function pattern ---
    print()
    print("=" * 100)
    print("ANALYSIS: LOOKING FOR PATTERNS IN SECTIONS 5, 25, 26 vs OTHERS")
    print("=" * 100)

    for target_sec_num in [4, 5, 6, 7, 8, 25, 26]:
        if target_sec_num > len(section_hdrs):
            continue
        sec = section_hdrs[target_sec_num - 1]
        sec_sym = section_sym_map.get(target_sec_num)
        ext_syms = section_externals.get(target_sec_num, [])

        print(f"\n  --- Section {target_sec_num}: {sec['name']} ---")
        print(f"  Size: {sec['raw_size']}, Relocs: {sec['num_relocs']}, Chars: 0x{sec['chars']:08X}")
        if sec_sym:
            print(f"  Section symbol: [{sec_sym['index']}] {sec_sym['name']} (aux={sec_sym['num_aux']})")
            if sec_sym['num_aux'] > 0:
                aux_off = symtab_offset + (sec_sym['index'] + 1) * 18
                aux_bytes = data[aux_off:aux_off+18]
                length, nr, nl, ck, assoc_num, sel = struct.unpack_from("<IHHIHB", aux_bytes, 0)
                sel_name = COMDAT_SELECTION.get(sel, f"UNKNOWN({sel})")
                print(f"  COMDAT aux: length={length}, relocs={nr}, linenums={nl}, "
                      f"checksum=0x{ck:08X}, assoc={assoc_num}, selection={sel_name}")
        print(f"  EXTERNAL symbols in this section:")
        for es in ext_syms:
            print(f"    [{es['index']}] {es['name']}")
        if not ext_syms:
            print(f"    (NONE - this section has no EXTERNAL symbol!)")

    # --- Raw hex dump ---
    print()
    print("=" * 100)
    print("RAW HEX OF FIRST 64 BYTES OF SECTIONS 4, 5, 25, 26:")
    print("=" * 100)
    for target_sec_num in [4, 5, 25, 26]:
        if target_sec_num > len(section_hdrs):
            continue
        sec = section_hdrs[target_sec_num - 1]
        start = sec['raw_ptr']
        end = min(start + 64, start + sec['raw_size'])
        chunk = data[start:end]
        print(f"\n  Section {target_sec_num} ({sec['name']}) raw data at 0x{start:06X}:")
        for row in range(0, len(chunk), 16):
            hex_part = " ".join(f"{b:02X}" for b in chunk[row:row+16])
            ascii_part = "".join(chr(b) if 32 <= b < 127 else '.' for b in chunk[row:row+16])
            print(f"    {row:04X}: {hex_part:<48s}  {ascii_part}")

    # --- Full ordered listing ---
    print()
    print("=" * 100)
    print("FULL ORDERED SYMBOL LISTING (condensed):")
    print("=" * 100)
    for s in symbols:
        marker = ""
        if s['storage_class'] == 3 and s['value'] == 0 and s['section'] > 0:
            marker = " <<< SECTION DEF"
        elif s['storage_class'] == 2 and s['section'] > 0:
            marker = " <<< EXTERNAL DEFINED"
        elif s['storage_class'] == 2 and s['section'] == 0:
            marker = " <<< EXTERNAL UNDEF"
        print(f"  [{s['index']:4d}] sect={s['section']:3d}  val=0x{s['value']:08X}  "
              f"cls={s['sc_name']:<12s}  {s['name']}{marker}")


if __name__ == "__main__":
    main()
