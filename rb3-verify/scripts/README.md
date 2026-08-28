# Xenia headless scripted-input format (reverse-engineered)

Two mechanisms exist (src/xenia/hid/nop/nop_input_driver.cc):

## 1. Time-based (`--scripted_input="..."`)  — USED HERE
Comma-separated events: `<time>:<button>[:<holdDuration>]`
- time/hold units: `s` (seconds) or `ms` or bare (ms).  e.g. `14s:A`, `500ms:START`
- button names (case-insensitive): A/CONFIRM, B, X, Y, START, BACK,
  UP/DOWN/LEFT/RIGHT (dpad), LB, RB, LS, RS, GUIDE, NONE
- combos with `+`:  `A+START`
- **pad targeting (added in this work):** suffix `@N` selects controller port
  0 or 1.  e.g. `20s:A@1` presses A on the SECOND controller.
  Default pad is 0.  Example 2-player: `"14s:A@0, 30s:A@1, 34s:DOWN@1, 38s:A@1"`

## 2. Screen-aware (`--scripted_input_file=<path>`) — DC3-ONLY, unusable for RB3
`wait_screen <name>` / `+<frames> <button>`.  Reads DC3 guest addresses
(TheUI 0x82F1A8E0) and has DC3 screen names hard-coded. Does NOT work for RB3
(different UIManager address + screen names). RB3 needs the time-based path.
