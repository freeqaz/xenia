# tools/rb3

RB3 helpers, moved from `rb3-verify/` (2026-10-02 fork cleanup). The retail
`.xex` and the frame dumps that used to sit next to them were purged from
history; build inputs with `tools/fork-regress/build_content.sh` instead.

| Path | What |
|---|---|
| `patch/apply_dirtydisc_bypass.py` | Dirty-disc bypass patcher for the clean TU5 `.xex` (the "nodd" builds). |
| `patch/apply_same_instrument_clean_tu5.py` | Same-instrument patch for clean TU5. **Not reproducible**: its write list lived in a deleted rb3-xenon worktree. |
| `input/*.txt` | `--scripted_input` sequences for RB3 / RB3DX; `input/README.md` documents both scripted-input formats. |
