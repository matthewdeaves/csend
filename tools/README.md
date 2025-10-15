# CSend Tools Directory

This directory contains development tools and utilities for the CSend project.

## Tools Overview

### Log Analysis Tools

#### 1. `filter_logs.sh` (in `../scripts/`)
Shell script for filtering and analyzing CSend log files.

**Quick usage**:
```bash
# Get summary
scripts/filter_logs.sh -s csend_posix.log

# Show errors only
scripts/filter_logs.sh -e csend_posix.log

# Filter by peer
scripts/filter_logs.sh -p MacTCP csend_posix.log
```

See `../scripts/FILTER_LOGS_EXAMPLES.md` for complete usage guide.

#### 2. `csend_log_mcp.py`
**Project-specific** MCP (Model Context Protocol) server that provides Claude with structured access to log analysis tools.

**Features**:
- Intelligent log filtering and searching
- Cross-platform log comparison
- Peer activity analysis
- Test result analysis
- Summary statistics

**Setup**: See `MCP_SETUP.md` for installation instructions.

**Security**: This MCP validates it's running in the CSend project and will refuse to run elsewhere.

### Python Client Libraries

#### 3. `csend_client.py`
Python client library for programmatic interaction with CSend instances via machine mode.

**Features**:
- Send/receive messages
- Manage peers
- Event streaming
- Correlation ID tracking

**Usage**:
```python
from csend_client import CSendClient

client = CSendClient()
client.start_csend("myuser")
client.send_message("peer1", "Hello!")
```

#### 4. `csend_chatbot.py`
Claude Haiku chatbot integration for CSend.

**Usage**:
```bash
export ANTHROPIC_API_KEY="your-key"
scripts/run_machine_mode.sh --chatbot
```

### Testing and Validation

#### 5. `test_machine_mode.py`
Automated testing for machine mode JSON API.

**Usage**:
```bash
python3 tools/test_machine_mode.py
```

#### 6. `test_mcp.sh`
Validation script for the CSend Log Analyzer MCP setup.

**Usage**:
```bash
tools/test_mcp.sh
```

## Configuration Files

- `csend_log_mcp_config.json` - Example MCP configuration for Claude
- `MCP_SETUP.md` - Detailed MCP installation instructions

## Log Analysis Workflow

### For Manual Debugging

1. **Start with summary**:
   ```bash
   scripts/filter_logs.sh -s csend_posix.log
   ```

2. **Find errors**:
   ```bash
   scripts/filter_logs.sh -e -C 3 csend_posix.log
   ```

3. **Track specific peer**:
   ```bash
   scripts/filter_logs.sh -p MacTCP -x csend_posix.log
   ```

4. **Analyze test results**:
   ```bash
   scripts/filter_logs.sh -t csend_posix.log
   ```

### For Claude-Assisted Debugging

With the MCP installed, Claude can analyze logs automatically:

```
User: "What errors occurred during the last test?"
Claude: [Uses MCP to filter logs and analyze test results]

User: "Compare network issues across all platforms"
Claude: [Uses MCP to compare POSIX, MacTCP, and OpenTransport logs]
```

## Development

### Adding New Log Analysis Features

1. Update `filter_logs.sh` with new functionality
2. Update `csend_log_mcp.py` to expose it via MCP (optional)
3. Update `FILTER_LOGS_EXAMPLES.md` with examples
4. Test with `test_mcp.sh`

### MCP Tool Development

The MCP server (`csend_log_mcp.py`) provides these tools:
- `log-summary` - Statistical overview
- `filter-logs` - Advanced filtering
- `find-errors` - Error analysis
- `analyze-peer` - Per-peer activity
- `analyze-test` - Test result analysis
- `compare-platforms` - Cross-platform comparison

Add new tools by:
1. Adding to `handle_list_tools()`
2. Implementing in `handle_call_tool()`
3. Updating `MCP_SETUP.md` documentation

## Project-Specific Design

The CSend log analysis tools are intentionally project-specific:

1. **Validation**: MCP checks for CSend project markers
2. **Log Format**: Understands CSend's categorized logging
3. **Categories**: NETWORKING, DISCOVERY, PEER_MGMT, etc.
4. **Test Format**: Recognizes CSend test markers
5. **Peer Format**: Parses `username@IP` format

This design ensures the tools provide accurate, context-aware analysis for CSend debugging.

## Requirements

### For Shell Script Tools
- Bash 4.0+
- Standard Unix utilities (grep, awk, wc, etc.)

### For Python Tools
- Python 3.8+
- `mcp` package (for MCP server): `pip install mcp`
- `anthropic` package (for chatbot): `pip install anthropic`

### For MCP Usage
- Claude Desktop or Claude Code with MCP support
- Properly configured `claude_desktop_config.json` or `.claude/mcp.json`

## Quick Start

1. **Run CSend to generate logs**:
   ```bash
   ./build/posix/csend_posix myuser
   ```

2. **Analyze logs**:
   ```bash
   scripts/filter_logs.sh -s csend_posix.log
   ```

3. **Set up MCP** (optional):
   ```bash
   tools/test_mcp.sh  # Validate setup
   # Follow MCP_SETUP.md for Claude integration
   ```

4. **Use with Claude**:
   ```
   "Show me a summary of all CSend logs"
   "What networking errors occurred?"
   "Compare test results across platforms"
   ```

## Files in This Directory

```
tools/
├── README.md                      # This file
├── MCP_SETUP.md                   # MCP installation guide
├── csend_log_mcp.py              # MCP server (main tool)
├── csend_log_mcp_config.json     # Example MCP config
├── test_mcp.sh                    # MCP validation script
├── csend_client.py               # Python client library
├── csend_chatbot.py              # AI chatbot integration
└── test_machine_mode.py          # Machine mode tests
```

## Related Documentation

- `../scripts/FILTER_LOGS_EXAMPLES.md` - Comprehensive filtering examples
- `../docs/LOGGING.md` - Logging system documentation
- `../CLAUDE.md` - Project-level Claude instructions
- `MCP_SETUP.md` - MCP setup and usage guide
