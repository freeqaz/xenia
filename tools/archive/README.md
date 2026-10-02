# tools/archive

Scripts kept for the record, not maintained (FORK_CLEANUP_PLAN.md §2.7,
bucket 4). Paths and cvars inside them are as of February 2026.

| Script | Why archived |
|---|---|
| `dc3_nui_cutover_gate.sh` | Gate for the Feb-2026 NUI resolver cutover, which is done. |
| `dc3_crt_bisect.sh` | Bisected the DC3 decomp CRT constructor blocker, resolved in session 37. Hardcodes host paths. |
| `dc3_extract_addresses.py` | Address extraction superseded by the patch manifest's `address_catalog`. |
| `analyze_poolalloc.py` | DC3 pool-allocator analysis; belongs in dc3-decomp. Hardcodes host paths. |
| `dc3_gdb_rsp_snapshot_bridge.sh` | Bridge for the early GDB RSP mock; superseded by the in-process RSP server (`docs/fork/debug/gdb_debugging.md`). |
