#!/usr/bin/env bash
#
# dc3_crt_bisect.sh -- Binary search for heap-corrupting CRT constructor(s)
#
# The DC3 decomp XEX has 390 CRT constructor table entries (indices 0-389),
# of which 364 are valid (26 are nullified as out-of-bounds pointers to
# 0x82000000). During CRT static initialization, the guest heap gets
# corrupted (RtlpCoalesceFreeBlocks crash with "Sord" + 0xFEEEFEEE pattern).
#
# xenia-headless has a --dc3_crt_bisect_max=N flag that nullifies all CRT
# constructors with index > N. This script uses binary search over that
# parameter to find the smallest N that still crashes, narrowing down
# which constructor(s) corrupt the heap.
#
# Usage:
#   ./dc3_crt_bisect.sh [MIN] [MAX] [TIMEOUT_MS]
#
# Arguments:
#   MIN         - Lower bound of search range (default: 0)
#   MAX         - Upper bound of search range (default: 389)
#   TIMEOUT_MS  - Timeout per test run in ms (default: 20000)
#
# Detection logic:
#   - Exit code 0 (clean timeout via std::_Exit) = NO CRASH (success)
#   - Exit code != 0 (signal 11 / SIGSEGV, or other nonzero) = CRASH
#   - Additionally checks log for "RtlpCoalesceFreeBlocks" as confirmation
#
# Output:
#   Results logged to /tmp/bisect_results.txt
#   Final range printed to stdout

set -euo pipefail

# ── Configuration ──────────────────────────────────────────────────────

XENIA_DIR="/home/free/code/milohax/xenia"
XENIA_BIN="${XENIA_DIR}/build/bin/Linux/Checked/xenia-headless"
XEX_PATH="/home/free/code/milohax/dc3-decomp/build/373307D9/default.xex"
RESULTS_FILE="/tmp/bisect_results.txt"
LOG_DIR="/tmp/bisect_logs"

LO=${1:-0}
HI=${2:-389}
TIMEOUT_MS=${3:-20000}

# Add a few seconds of wall-clock margin beyond the guest timeout so the
# process has time to print "TIMEOUT:" and call std::_Exit(0) cleanly.
# If it hasn't exited by then, `timeout` kills it (exit code 124).
WALL_TIMEOUT_SEC=$(( (TIMEOUT_MS / 1000) + 10 ))

# ── Helpers ────────────────────────────────────────────────────────────

mkdir -p "${LOG_DIR}"

log() {
    local msg="[$(date '+%H:%M:%S')] $*"
    echo "$msg"
    echo "$msg" >> "${RESULTS_FILE}"
}

# Run xenia with a given bisect_max value.
# Returns 0 if the run was a clean timeout (no crash), 1 if it crashed.
run_test() {
    local n=$1
    local logfile="${LOG_DIR}/bisect_${n}.log"

    # Clean up shared memory from previous runs
    rm -f /dev/shm/xenia_* 2>/dev/null || true

    # Run with wall-clock timeout as a safety net.
    # The emulator's own --headless_timeout_ms handles the normal timeout;
    # the outer `timeout` catches hangs or cases where the emulator doesn't
    # exit cleanly.
    local exit_code=0
    timeout "${WALL_TIMEOUT_SEC}" \
        "${XENIA_BIN}" \
        --gpu=null \
        --target="${XEX_PATH}" \
        --headless_timeout_ms="${TIMEOUT_MS}" \
        --dc3_crt_bisect_max="${n}" \
        > "${logfile}" 2>&1 \
    || exit_code=$?

    # Classify the result:
    #   exit_code == 0    -> clean timeout (std::_Exit(0)), no crash
    #   exit_code == 124  -> wall-clock timeout (killed by `timeout` command)
    #   exit_code == 139  -> SIGSEGV (128 + 11)
    #   exit_code == 134  -> SIGABRT (128 + 6)
    #   any other nonzero -> some other crash/error

    local crashed=0
    local reason=""

    # Detect the SPECIFIC heap corruption crash we're bisecting.
    # Xenia catches SIGSEGV internally, logs diagnostics, and keeps running
    # until timeout. So exit_code==0 + TIMEOUT does NOT mean "no crash".
    #
    # We ONLY count crashes at the RtlpCoalesceFreeBlocks function (address
    # 0x830DC644) as the target "crash" for bisect purposes. The crash dump
    # outputs raw addresses, not function names, so we grep for the address.
    # Other crashes (null vtable, missing globals) happen at ANY bisect level
    # and would confuse the binary search.
    if grep -q "0x830DC644\|830DC644\|RtlpCoalesceFreeBlocks\|CoalesceFree" "${logfile}" 2>/dev/null; then
        crashed=1
        reason="heap_crash"
        if grep -q "536F7264\|FEEEFEEE" "${logfile}" 2>/dev/null; then
            reason="${reason}+sord_pattern"
        fi
    else
        reason="no_heap_crash"
        # Note other crashes for informational purposes
        if grep -q "CRASH DUMP\|CRASH:" "${logfile}" 2>/dev/null; then
            reason="${reason}+other_crash"
        fi
        if grep -q "TIMEOUT:" "${logfile}" 2>/dev/null; then
            reason="${reason}+timeout"
        fi
    fi

    log "  N=${n}: exit=${exit_code} crashed=${crashed} reason=${reason}"

    return ${crashed}
}

# ── Pre-flight checks ─────────────────────────────────────────────────

if [ ! -x "${XENIA_BIN}" ]; then
    echo "ERROR: xenia-headless not found at ${XENIA_BIN}"
    echo "Build it with: cd ${XENIA_DIR}/build && make xenia-headless config=checked_linux -j\$(nproc)"
    exit 1
fi

if [ ! -f "${XEX_PATH}" ]; then
    echo "ERROR: XEX not found at ${XEX_PATH}"
    exit 1
fi

# ── Initialize results file ───────────────────────────────────────────

{
    echo "========================================"
    echo "DC3 CRT Constructor Binary Search"
    echo "Started: $(date)"
    echo "Range: ${LO} to ${HI}"
    echo "Timeout per test: ${TIMEOUT_MS}ms"
    echo "Wall timeout: ${WALL_TIMEOUT_SEC}s"
    echo "========================================"
} > "${RESULTS_FILE}"

# ── Sanity checks at boundaries ───────────────────────────────────────

log ""
log "=== Phase 0: Boundary validation ==="

# First, verify that the full range (max=389 or whatever HI is) actually
# crashes. If it doesn't, there's nothing to bisect.
log "Testing upper bound (N=${HI}) -- expect CRASH..."
if run_test "${HI}"; then
    log "WARNING: N=${HI} did NOT crash. The full range doesn't reproduce the bug."
    log "This could mean:"
    log "  - The crash is non-deterministic (try increasing timeout)"
    log "  - The heap corruption happens after CRT init (not a constructor issue)"
    log "  - The bisect_max flag isn't working as expected"
    log ""
    log "Proceeding anyway, but results may be unreliable."
    UPPER_CRASHES=0
else
    log "Confirmed: N=${HI} crashes (expected)."
    UPPER_CRASHES=1
fi

# Then verify that N=0 (only constructor 0) does NOT crash.
# If it does, the very first constructor is the culprit.
log ""
log "Testing lower bound (N=${LO}) -- expect NO crash..."
if run_test "${LO}"; then
    log "Confirmed: N=${LO} does not crash (expected)."
    LOWER_OK=1
else
    log "INTERESTING: N=${LO} also crashes!"
    log "Constructor 0 alone may be sufficient to trigger the bug."
    log "Or the crash happens even with 0 constructors (different root cause)."
    LOWER_OK=0
fi

# If the lower bound crashes, try N=0 with no constructors at all by testing
# a value that should exclude all entries. But our flag uses index-based
# cutoff, so bisect_max=0 still allows index 0. We can't go below 0 with
# the current flag, so report this edge case.
if [ "${LOWER_OK}" -eq 0 ]; then
    log ""
    log "RESULT: Even N=${LO} crashes. Cannot narrow further with current flag."
    log "Consider: the crash may not be caused by CRT constructors at all,"
    log "or constructor index ${LO} itself is the culprit."
    log ""
    log "Final answer: constructor index ${LO} (or a non-constructor cause)"
    exit 0
fi

if [ "${UPPER_CRASHES}" -eq 0 ]; then
    log ""
    log "RESULT: Upper bound N=${HI} does not crash. Nothing to bisect."
    log "The crash may be non-deterministic or caused by something else."
    exit 0
fi

# ── Binary search ─────────────────────────────────────────────────────

log ""
log "=== Phase 1: Binary search ==="
log "Finding the lowest N that still crashes..."
log "Search space: [${LO}, ${HI}]"

lo=${LO}
hi=${HI}
iteration=0

while [ $(( hi - lo )) -gt 1 ]; do
    mid=$(( (lo + hi) / 2 ))
    iteration=$(( iteration + 1 ))
    remaining=$(( hi - lo ))

    log ""
    log "--- Iteration ${iteration}: lo=${lo} hi=${hi} mid=${mid} (range=${remaining}) ---"

    if run_test "${mid}"; then
        # No crash at mid: the corrupting constructor has index > mid
        log "  -> No crash at N=${mid}. Culprit is in (${mid}, ${hi}]"
        lo=${mid}
    else
        # Crash at mid: the corrupting constructor has index <= mid
        log "  -> CRASH at N=${mid}. Culprit is in [${lo}, ${mid}]"
        hi=${mid}
    fi
done

# ── Narrowed range ────────────────────────────────────────────────────

log ""
log "=== Phase 2: Narrowed result ==="
log "Binary search complete after ${iteration} iterations."
log ""
log "The heap-corrupting constructor is at index ${hi}."
log "  - N=${lo} (indices 0..${lo}): NO CRASH"
log "  - N=${hi} (indices 0..${hi}): CRASH"
log ""
log "This means constructor #${hi} is the first one that triggers heap corruption."
log "It could be:"
log "  a) Constructor #${hi} itself corrupts the heap"
log "  b) Constructor #${hi} depends on state from an earlier constructor that"
log "     sets up conditions for corruption when #${hi} runs"
log ""

# ── Phase 3: Confirmation and neighborhood scan ──────────────────────

log "=== Phase 3: Confirmation ==="
log "Re-testing N=${hi} to confirm crash is deterministic..."

CONFIRM_CRASH=0
for attempt in 1 2 3; do
    if run_test "${hi}"; then
        log "  Attempt ${attempt}: no crash (FLAKY!)"
    else
        log "  Attempt ${attempt}: crash confirmed"
        CONFIRM_CRASH=$(( CONFIRM_CRASH + 1 ))
    fi
done

log ""
if [ "${CONFIRM_CRASH}" -eq 3 ]; then
    log "Result: DETERMINISTIC crash at constructor index ${hi} (3/3 repros)"
elif [ "${CONFIRM_CRASH}" -gt 0 ]; then
    log "Result: FLAKY crash at constructor index ${hi} (${CONFIRM_CRASH}/3 repros)"
    log "The crash may be timing-dependent or involve multiple constructors."
else
    log "Result: Could not reproduce crash at index ${hi} in confirmation phase!"
    log "The original binary search may have hit a flaky case."
fi

log ""
log "========================================"
log "DONE. Results saved to ${RESULTS_FILE}"
log "Logs for each test: ${LOG_DIR}/bisect_*.log"
log ""
log "Next steps:"
log "  1. Look at the CRT table entry at index ${hi}:"
log "     grep 'CRT\\[.*${hi}\\]' ${LOG_DIR}/bisect_${hi}.log"
log "  2. Find what function that address corresponds to in the MAP file"
log "  3. Check if that function calls any unresolved symbols"
log "========================================"
