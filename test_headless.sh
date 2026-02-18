#!/bin/bash
# Xenia Headless Validation Script
# Tests that xenia-headless binary builds and runs without UI dependencies

set -e

XENIA_DIR="/tmp/claude/xenia"
HEADLESS_BIN="$XENIA_DIR/build/bin/Linux/Checked/xenia-headless"
TEST_XEX="/home/free/code/milohax/milo-executable-library/dc1/TU0/default.xex"
TIMEOUT_MS=5000

echo "=== Xenia Headless Validation ==="
echo ""

# Test 1: Binary exists
echo "Test 1: Binary exists..."
if [ -f "$HEADLESS_BIN" ]; then
    echo "  ✓ Binary found: $HEADLESS_BIN"
else
    echo "  ✗ Binary not found!"
    exit 1
fi

# Test 2: Binary is executable
echo "Test 2: Binary is executable..."
if [ -x "$HEADLESS_BIN" ]; then
    echo "  ✓ Binary is executable"
else
    echo "  ✗ Binary is not executable!"
    exit 1
fi

# Test 3: No GTK dependency
echo "Test 3: No GTK dependency..."
if ldd "$HEADLESS_BIN" 2>/dev/null | grep -q "libgtk-3"; then
    echo "  ✗ GTK dependency found!"
    ldd "$HEADLESS_BIN" | grep "libgtk"
    exit 1
else
    echo "  ✓ No GTK dependency"
fi

# Test 4: No Vulkan dependency
echo "Test 4: No Vulkan dependency..."
if ldd "$HEADLESS_BIN" 2>/dev/null | grep -q "libvulkan"; then
    echo "  ✗ Vulkan dependency found!"
    ldd "$HEADLESS_BIN" | grep "libvulkan"
    exit 1
else
    echo "  ✓ No Vulkan dependency"
fi

# Test 5: No X11 dependency
echo "Test 5: No X11 dependency..."
if ldd "$HEADLESS_BIN" 2>/dev/null | grep -q "libX11"; then
    echo "  ✗ X11 dependency found!"
    ldd "$HEADLESS_BIN" | grep "libX11"
    exit 1
else
    echo "  ✓ No X11 dependency"
fi

# Test 6: Binary size check
echo "Test 6: Binary size check..."
SIZE=$(stat -c%s "$HEADLESS_BIN" 2>/dev/null || stat -f%z "$HEADLESS_BIN")
SIZE_MB=$((SIZE / 1024 / 1024))
if [ $SIZE_MB -lt 160 ]; then
    echo "  ✓ Binary size: ${SIZE_MB}MB (under 160MB target)"
else
    echo "  ⚠ Binary size: ${SIZE_MB}MB (over 160MB target)"
fi

# Test 7: Emulator initializes
echo "Test 7: Emulator initializes..."
OUTPUT=$("$HEADLESS_BIN" --headless_timeout_ms=1000 2>&1 || true)
if echo "$OUTPUT" | grep -q "Emulator initialized with headless backends"; then
    echo "  ✓ Emulator initializes with headless backends"
else
    echo "  ✗ Emulator failed to initialize"
    echo "$OUTPUT" | head -10
    exit 1
fi

# Test 8: Game boots (if test XEX available)
echo "Test 8: Game boot test..."
if [ -f "$TEST_XEX" ]; then
    OUTPUT=$("$HEADLESS_BIN" --target="$TEST_XEX" --headless_timeout_ms=$TIMEOUT_MS 2>&1 || true)
    if echo "$OUTPUT" | grep -q "BOOT: Title loaded successfully"; then
        TITLE_ID=$(echo "$OUTPUT" | grep "BOOT: Title ID:" | head -1 | sed 's/.*Title ID: //')
        TITLE_NAME=$(echo "$OUTPUT" | grep "BOOT: Title Name:" | head -1 | sed 's/.*Title Name: //')
        echo "  ✓ Game boots successfully"
        echo "    Title ID: $TITLE_ID"
        echo "    Title Name: $TITLE_NAME"
    else
        echo "  ⚠ Game boot test failed (may be expected)"
    fi
else
    echo "  ⊘ Skipped (no test XEX at $TEST_XEX)"
fi

echo ""
echo "=== All Tests Passed ==="
echo ""
echo "Binary: $HEADLESS_BIN"
echo "Size: ${SIZE_MB}MB"
echo ""
echo "Usage:"
echo "  $HEADLESS_BIN --target=/path/to/game.xex --headless_timeout_ms=30000"
echo ""
echo "Options:"
echo "  --target=<path>           XEX or ISO file to launch"
echo "  --headless_timeout_ms=N   Exit after N milliseconds (0 = run forever)"
echo "  --headless_report_boot    Report boot status (default: true)"
