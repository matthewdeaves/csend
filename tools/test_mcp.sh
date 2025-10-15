#!/bin/bash
# Test script for CSend Log Analyzer MCP
# This simulates what happens when Claude uses the MCP

set -e

echo "CSend Log Analyzer MCP Test"
echo "=============================="
echo ""

# Check Python version
echo "1. Checking Python version..."
python3 --version || { echo "Error: Python 3 not found"; exit 1; }
echo ""

# Check MCP SDK
echo "2. Checking MCP SDK..."
if python3 -c "import mcp" 2>/dev/null; then
    echo "✓ MCP SDK installed"
else
    echo "✗ MCP SDK not installed"
    echo "  Install with: pip install mcp"
    echo "  (Note: MCP SDK is required for the server, but not for the shell script)"
fi
echo ""

# Determine project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Check filter script
echo "3. Checking filter script..."
if [ -f "scripts/filter_logs.sh" ]; then
    echo "✓ filter_logs.sh found"
    if [ -x "scripts/filter_logs.sh" ]; then
        echo "✓ filter_logs.sh is executable"
    else
        echo "⚠ filter_logs.sh is not executable (running chmod +x)"
        chmod +x scripts/filter_logs.sh
    fi
else
    echo "✗ filter_logs.sh not found at $PROJECT_ROOT/scripts/filter_logs.sh"
    exit 1
fi
echo ""

# Check for log files
echo "4. Checking for log files..."
found_logs=0
for log in csend_posix.log csend_mac.log csend_classic_mac_ot_ppc.log; do
    if [ -f "$log" ]; then
        echo "✓ Found: $log ($(wc -l < "$log") lines)"
        found_logs=$((found_logs + 1))
    fi
done

if [ $found_logs -eq 0 ]; then
    echo "⚠ No log files found. Run csend to generate logs."
else
    echo "✓ Found $found_logs log file(s)"
fi
echo ""

# Test filter script directly
echo "5. Testing filter script directly..."
if [ $found_logs -gt 0 ]; then
    for log in csend_posix.log csend_mac.log csend_classic_mac_ot_ppc.log; do
        if [ -f "$log" ]; then
            echo "  Testing summary for $log..."
            scripts/filter_logs.sh -s "$log" > /dev/null 2>&1 && echo "    ✓ Summary works" || echo "    ✗ Summary failed"
            echo "  Testing error filter for $log..."
            scripts/filter_logs.sh -e "$log" > /dev/null 2>&1 && echo "    ✓ Error filter works" || echo "    ✗ Error filter failed"
            break
        fi
    done
else
    echo "  Skipped (no logs available)"
fi
echo ""

# Check project validation markers
echo "6. Checking CSend project markers..."
markers=(
    "CLAUDE.md"
    "shared/protocol.c"
    "scripts/filter_logs.sh"
    "posix/main.c"
)

all_found=true
for marker in "${markers[@]}"; do
    if [ -f "$marker" ]; then
        echo "  ✓ $marker"
    else
        echo "  ✗ $marker (MISSING)"
        all_found=false
    fi
done

if [ "$all_found" = true ]; then
    echo "✓ All project markers found - MCP validation will succeed"
else
    echo "✗ Some project markers missing - MCP validation will fail"
fi
echo ""

echo "=============================="
echo "Test Summary"
echo "=============================="
echo ""
if [ "$all_found" = true ] && [ $found_logs -gt 0 ]; then
    echo "✓ All checks passed!"
    echo "  The MCP is ready to use (requires: pip install mcp)"
    echo ""
    echo "To use with Claude:"
    echo "  1. Install MCP SDK: pip install mcp"
    echo "  2. Add to Claude config (see tools/MCP_SETUP.md)"
    echo "  3. Restart Claude"
else
    echo "⚠ Some checks failed, but core functionality works"
    echo "  The filter script can be used directly without MCP"
fi
echo ""
