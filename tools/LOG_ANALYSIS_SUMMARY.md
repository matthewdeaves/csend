# CSend Log Analysis Tools - Summary

## Overview

The CSend project now includes comprehensive log analysis capabilities designed specifically for debugging the peer-to-peer chat application across multiple platforms (POSIX, MacTCP, OpenTransport).

## What Was Improved

### 1. Enhanced Shell Script (`scripts/filter_logs.sh`)

**New capabilities added**:
- **Summary statistics** (`-s`): Overview of errors, warnings, peers, test results
- **Peer filtering** (`-p`): Track specific peer activity
- **Test filtering** (`-t`): Focus on test execution
- **Time filtering** (`-T`): Filter by timestamp patterns
- **Context lines** (`-C`): Show surrounding lines around matches
- **Line limiting** (`-n`): First/last N lines
- **Debug exclusion** (`-x`): Remove DEBUG noise

**Example improvements**:
```bash
# Before: Only basic level/category filtering
scripts/filter_logs.sh -c NETWORKING app_posix.log

# After: Rich filtering capabilities
scripts/filter_logs.sh -s csend_posix.log           # Summary
scripts/filter_logs.sh -p MacTCP -x csend_posix.log # Peer analysis
scripts/filter_logs.sh -C 3 -e csend_posix.log      # Errors with context
```

### 2. MCP Server (`tools/csend_log_mcp.py`)

**Project-specific Model Context Protocol server** that enables Claude to intelligently analyze logs:

**Features**:
- 6 specialized tools for log analysis
- Auto-detection of most recent logs
- Cross-platform log comparison
- Project validation (CSend-only)

**Tools provided**:
1. `log-summary` - Statistical overview
2. `filter-logs` - Advanced multi-criteria filtering
3. `find-errors` - Error analysis with context
4. `analyze-peer` - Per-peer activity tracking
5. `analyze-test` - Test result analysis
6. `compare-platforms` - Cross-platform comparison

**Security**: Validates project structure to ensure it only runs in CSend directory.

### 3. Documentation

Created comprehensive documentation:
- `scripts/FILTER_LOGS_EXAMPLES.md` - Detailed usage examples
- `tools/MCP_SETUP.md` - MCP installation guide
- `tools/README.md` - Tools directory overview
- `tools/LOG_ANALYSIS_SUMMARY.md` - This file
- Updated `CLAUDE.md` - Project-level integration

### 4. Testing

- `tools/test_mcp.sh` - Validation script for MCP setup
- Tests all components: Python, MCP SDK, filter script, logs, project markers
- Provides clear feedback on readiness

## Usage Examples

### Manual Debugging

```bash
# Get overview
scripts/filter_logs.sh -s csend_posix.log

# Find connection issues
scripts/filter_logs.sh -w -c NETWORKING -C 5 csend_posix.log

# Track problematic peer
scripts/filter_logs.sh -p 10.188.1.213 -x csend_posix.log

# Compare test results
scripts/filter_logs.sh -t csend_posix.log > posix_test.txt
scripts/filter_logs.sh -t csend_mac.log > mactcp_test.txt
diff posix_test.txt mactcp_test.txt
```

### With Claude (MCP Enabled)

```
User: "Show me a summary of the POSIX log"
Claude: [Uses log-summary tool]
       → Shows: 580 lines, 20 errors, 20 warnings, 2 peers detected

User: "What errors occurred with MacTCP peer?"
Claude: [Uses analyze-peer with peer="MacTCP" + error filtering]
       → Shows: Connection refused errors, timestamps, context

User: "Compare test results across all platforms"
Claude: [Uses compare-platforms tool]
       → Shows: Side-by-side test summaries for POSIX, MacTCP, OpenTransport
```

## Design Philosophy

### Project-Specific
- Understands CSend log format: `[LEVEL][CATEGORY]`
- Recognizes test markers: `TEST_R`, `AUTOMATED TEST`
- Parses peer format: `username@IP`
- Validates CSend project structure

### Layered Approach
1. **Shell script** - Fast, universal, no dependencies
2. **MCP server** - Structured access for AI assistance
3. **Documentation** - Examples and guides for humans

### Debug-Friendly
- Context lines around errors
- Time-based filtering
- Peer-specific views
- Test isolation
- Cross-platform comparison

## Benefits for Debugging

### Before
- Manually grep through logs
- Hard to correlate errors across platforms
- Difficult to focus on specific peers
- Test results mixed with normal operation
- No quick overview of log health

### After
- One command for log overview
- Automated cross-platform comparison
- Peer-specific filtering
- Test-only views
- Claude can analyze logs interactively
- Context-aware error investigation

## Quick Start

1. **Validate setup**:
   ```bash
   tools/test_mcp.sh
   ```

2. **Try the shell script**:
   ```bash
   scripts/filter_logs.sh -s csend_posix.log
   ```

3. **Set up MCP** (optional):
   - Follow `tools/MCP_SETUP.md`
   - Restart Claude
   - Ask: "Show me a summary of CSend logs"

4. **Debug workflow**:
   ```bash
   # 1. Overview
   scripts/filter_logs.sh -s csend_posix.log

   # 2. Find errors
   scripts/filter_logs.sh -e -C 3 csend_posix.log

   # 3. Track peer
   scripts/filter_logs.sh -p MacTCP -x csend_posix.log

   # 4. Check tests
   scripts/filter_logs.sh -t csend_posix.log
   ```

## Files Created/Modified

**New files**:
- `scripts/filter_logs.sh` (enhanced)
- `scripts/FILTER_LOGS_EXAMPLES.md`
- `tools/csend_log_mcp.py`
- `tools/csend_log_mcp_config.json`
- `tools/MCP_SETUP.md`
- `tools/README.md`
- `tools/test_mcp.sh`
- `tools/LOG_ANALYSIS_SUMMARY.md`

**Modified files**:
- `CLAUDE.md` (added log analysis section)

## Requirements

**For shell script only**:
- Bash 4.0+
- Standard Unix utilities (grep, awk, etc.)

**For MCP server**:
- Python 3.8+
- `pip install mcp`
- Claude Desktop or Claude Code with MCP support

## Future Enhancements

Possible additions:
- Visualization of peer connections over time
- Anomaly detection (unusual patterns)
- Performance metrics extraction
- Log diff tool (compare before/after)
- Export to structured formats (JSON, CSV)

## Conclusion

The CSend log analysis tools provide a powerful, project-specific debugging aid that works both manually (shell script) and with AI assistance (MCP server). The tools understand CSend's logging format, peer structure, and test patterns, making debugging more efficient and accurate.

**Key Takeaway**: Whether you're debugging manually or with Claude's help, you now have intelligent, context-aware log analysis tools specifically designed for CSend.
