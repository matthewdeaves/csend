# CSend Log Analyzer MCP Setup

This document explains how to set up the CSend-specific log analyzer MCP (Model Context Protocol) server.

## What is this?

The CSend Log Analyzer MCP is a **project-specific** tool that provides structured log analysis capabilities to Claude during debugging sessions. It wraps the `filter_logs.sh` script and provides intelligent access to CSend logs.

**IMPORTANT**: This MCP is designed ONLY for the CSend project. It validates the project structure on startup and will refuse to run in other directories.

## Prerequisites

1. Python 3.8 or higher
2. MCP SDK for Python:
   ```bash
   pip install mcp
   ```

## Installation

### For Claude Desktop

1. Open your Claude Desktop configuration file:
   - macOS: `~/Library/Application Support/Claude/claude_desktop_config.json`
   - Linux: `~/.config/Claude/claude_desktop_config.json`
   - Windows: `%APPDATA%\Claude\claude_desktop_config.json`

2. Add the CSend log analyzer server to the `mcpServers` section:
   ```json
   {
     "mcpServers": {
       "csend-log-analyzer": {
         "command": "python3",
         "args": ["tools/csend_log_mcp.py"],
         "cwd": "/home/matt/macos9/shared/csend"
       }
     }
   }
   ```

3. Adjust the `cwd` path to match your CSend project location.

4. Restart Claude Desktop.

### For Claude Code

Add to your `.claude/mcp.json` in the CSend project root:
```json
{
  "mcpServers": {
    "csend-log-analyzer": {
      "command": "python3",
      "args": ["tools/csend_log_mcp.py"]
    }
  }
}
```

The `cwd` is automatically set to the project directory by Claude Code.

### Verification

After setup, you should see the CSend log analyzer tools available in Claude. You can verify by asking Claude:

"What MCP tools are available?"

You should see tools like:
- `log-summary`
- `filter-logs`
- `find-errors`
- `analyze-peer`
- `analyze-test`
- `compare-platforms`

## Available Tools

### 1. log-summary
Get a quick overview of log statistics including errors, warnings, peers, and test results.

**Example usage**: "Show me a summary of the POSIX log"

### 2. filter-logs
Advanced filtering with multiple criteria.

**Example usage**: "Filter the MacTCP logs to show only networking errors"

### 3. find-errors
Find and analyze error messages with context.

**Example usage**: "Find all errors in the OpenTransport log with 5 lines of context"

### 4. analyze-peer
Analyze all activity for a specific peer.

**Example usage**: "Show me all activity for peer MacTCP@10.188.1.213"

### 5. analyze-test
Analyze test execution results.

**Example usage**: "Show me the test results from the POSIX log"

### 6. compare-platforms
Compare logs across different platforms.

**Example usage**: "Compare test results across all platforms"

## Usage Examples in Claude

```
User: "Show me a summary of all logs"
Claude: [Uses log-summary tool on all available logs]

User: "What errors occurred with the MacTCP peer?"
Claude: [Uses analyze-peer with peer="MacTCP" and errors filter]

User: "Compare test results across platforms"
Claude: [Uses compare-platforms with filter_type="test"]

User: "Show networking warnings from the last test run"
Claude: [Uses filter-logs with category="NETWORKING", warnings_only=true, test_only=true]
```

## Security Note

This MCP is **project-specific** and includes validation to ensure it only runs in the CSend project directory. It will:

1. Check for required CSend files (CLAUDE.md, shared/protocol.c, etc.)
2. Validate the content of CLAUDE.md contains CSend-specific markers
3. Exit with an error if run outside the CSend project

This prevents accidental use of CSend-specific tooling in other projects.

## Troubleshooting

### "MCP SDK not installed"
```bash
pip install mcp
```

### "This MCP server is specifically designed for the CSend project"
You're trying to run it outside the CSend directory. The MCP must be run with `cwd` set to the CSend project root.

### "No log file found"
Logs are expected at:
- `csend_posix.log` (POSIX)
- `csend_mac.log` (MacTCP)
- `csend_classic_mac_ot_ppc.log` (OpenTransport)

Make sure the applications have been run and generated logs.

### Tool not appearing in Claude
1. Check your configuration file is valid JSON
2. Restart Claude Desktop/Code
3. Verify the path in `cwd` is correct
4. Check MCP SDK is installed: `python3 -c "import mcp"`

## Log File Locations

The MCP looks for logs in these locations:
- **POSIX**: `./csend_posix.log`
- **MacTCP**: `./csend_mac.log`
- **OpenTransport**: `./csend_classic_mac_ot_ppc.log`

All paths are relative to the project root.

## Development

The MCP wraps `scripts/filter_logs.sh`. Any improvements to that script automatically benefit the MCP.

To test the MCP directly:
```bash
cd /path/to/csend
python3 tools/csend_log_mcp.py
# Will wait for MCP protocol messages on stdin
```

For interactive testing, use the MCP inspector or Claude with MCP support.
