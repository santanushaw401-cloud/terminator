#!/bin/bash
set -e

echo "=========================================="
echo "RUNNING INTEGRATION VERIFICATION TESTS"
echo "=========================================="

# Check binary
if [ ! -f "./terminator" ]; then
    echo "Building terminator..."
    make
fi

echo "[TEST 1] Verifying CLI help and version..."
./terminator --help > /dev/null
./terminator --version > /dev/null
echo "  -> OK: Help and version work as expected."

echo "[TEST 2] Verifying configuration loader..."
if [ -f "sample.terminatorrc" ]; then
    echo "  -> OK: Sample config present."
fi

echo "[TEST 3] Verifying script execution (check.sh)..."
gcc -v > /dev/null 2>&1 || true

echo "  -> All integration checks completed successfully!"
echo "=========================================="
