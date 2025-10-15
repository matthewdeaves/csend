#!/usr/bin/env python3
"""
CSend Log Analysis MCP Server

This MCP server provides project-specific log filtering and analysis tools
for the CSend P2P chat application. It wraps the filter_logs.sh script and
provides structured access to log data for debugging and analysis.

IMPORTANT: This MCP is ONLY for use with the CSend project. It validates
the project structure and will refuse to run in other directories.

Usage:
    python3 csend_log_mcp.py
"""

import sys
import subprocess
import json
import os
from pathlib import Path
from typing import Optional

# Add MCP SDK to path if available
try:
    from mcp.server.fastmcp import FastMCP
except ImportError:
    print("Error: MCP SDK not installed. Install with: pip install mcp", file=sys.stderr)
    sys.exit(1)

# Get project root directory
SCRIPT_DIR = Path(__file__).parent.absolute()
PROJECT_ROOT = SCRIPT_DIR.parent
FILTER_SCRIPT = PROJECT_ROOT / "scripts" / "filter_logs.sh"

# Validate this is the CSend project
def validate_csend_project() -> bool:
    """
    Validate that we're running in the CSend project directory.
    Checks for key files and directories that uniquely identify CSend.
    """
    required_markers = [
        PROJECT_ROOT / "CLAUDE.md",  # Project-specific Claude instructions
        PROJECT_ROOT / "shared" / "protocol.c",  # CSend protocol implementation
        PROJECT_ROOT / "scripts" / "filter_logs.sh",  # This specific tool
        PROJECT_ROOT / "posix" / "main.c",  # POSIX implementation
    ]

    for marker in required_markers:
        if not marker.exists():
            return False

    # Extra validation: check for CSend-specific content in CLAUDE.md
    claude_md = PROJECT_ROOT / "CLAUDE.md"
    if claude_md.exists():
        try:
            content = claude_md.read_text()
            if "CSend" not in content or "peer-to-peer terminal chat" not in content.lower():
                return False
        except:
            return False

    return True

# Validate on startup
if not validate_csend_project():
    print(
        "ERROR: This MCP server is specifically designed for the CSend project.\n"
        "It appears you are not in the CSend project directory.\n"
        f"Current location: {PROJECT_ROOT}\n"
        "Required: A directory containing CSend project files (CLAUDE.md, shared/protocol.c, etc.)",
        file=sys.stderr
    )
    sys.exit(1)

# Default log file locations
DEFAULT_LOGS = {
    "posix": PROJECT_ROOT / "csend_posix.log",
    "mactcp": PROJECT_ROOT / "csend_mac.log",
    "opentransport": PROJECT_ROOT / "csend_classic_mac_ot_ppc.log"
}

# Initialize FastMCP server
mcp = FastMCP("csend-log-analyzer")


def run_filter(args: list) -> str:
    """Execute the filter_logs.sh script with given arguments."""
    if not FILTER_SCRIPT.exists():
        return f"Error: Filter script not found at {FILTER_SCRIPT}"

    try:
        result = subprocess.run(
            [str(FILTER_SCRIPT)] + args,
            capture_output=True,
            text=True,
            timeout=30
        )
        output = result.stdout
        if result.stderr:
            output += f"\n\nErrors:\n{result.stderr}"
        return output if output else "No output generated"
    except subprocess.TimeoutExpired:
        return "Error: Filter operation timed out"
    except Exception as e:
        return f"Error running filter: {str(e)}"


def find_log_file(platform: str = None) -> str:
    """Find the appropriate log file based on platform or auto-detect."""
    if platform:
        log_file = DEFAULT_LOGS.get(platform.lower())
        if log_file and log_file.exists():
            return str(log_file)

    # Auto-detect: find most recently modified log
    existing_logs = [(name, path) for name, path in DEFAULT_LOGS.items() if path.exists()]
    if existing_logs:
        most_recent = max(existing_logs, key=lambda x: x[1].stat().st_mtime)
        return str(most_recent[1])

    return None


@mcp.tool()
def log_summary(
    platform: Optional[str] = None,
    log_file: Optional[str] = None
) -> str:
    """Get a summary of log statistics including errors, warnings, peers, and test results.

    Args:
        platform: Platform log to analyze (posix, mactcp, or opentransport) - auto-detects if not specified
        log_file: Custom log file path (overrides platform selection)
    """
    # Determine log file
    target_log = log_file
    if not target_log:
        target_log = find_log_file(platform)

    if not target_log:
        return "Error: No log file found. Available logs: " + \
               ", ".join(f"{k}={v}" for k, v in DEFAULT_LOGS.items() if v.exists())

    if not Path(target_log).exists():
        return f"Error: Log file not found: {target_log}"

    return run_filter(["-s", target_log])


@mcp.tool()
def filter_logs(
    platform: Optional[str] = None,
    log_file: Optional[str] = None,
    level: Optional[str] = None,
    errors_only: bool = False,
    warnings_only: bool = False,
    exclude_debug: bool = False,
    category: Optional[str] = None,
    peer: Optional[str] = None,
    test_only: bool = False,
    time_pattern: Optional[str] = None,
    context_lines: Optional[int] = None,
    num_lines: Optional[int] = None
) -> str:
    """Filter and search log files with various criteria.

    Args:
        platform: Platform log to analyze (posix, mactcp, or opentransport)
        log_file: Custom log file path (overrides platform selection)
        level: Show logs at or above this level (ERROR, WARNING, INFO, DEBUG)
        errors_only: Show only ERROR level logs
        warnings_only: Show WARNING and ERROR logs
        exclude_debug: Exclude DEBUG logs (show INFO and above)
        category: Filter by log category (GENERAL, NETWORKING, DISCOVERY, PEER_MGMT, UI, PROTOCOL, SYSTEM, MESSAGING)
        peer: Filter by peer name or IP address
        test_only: Show only test-related messages
        time_pattern: Filter by time pattern (e.g., '16:44:2' for 16:44:2X)
        context_lines: Number of context lines around matches
        num_lines: Limit to first N lines (positive) or last N lines (negative)
    """
    # Determine log file
    target_log = log_file
    if not target_log:
        target_log = find_log_file(platform)

    if not target_log:
        return "Error: No log file found. Available logs: " + \
               ", ".join(f"{k}={v}" for k, v in DEFAULT_LOGS.items() if v.exists())

    if not Path(target_log).exists():
        return f"Error: Log file not found: {target_log}"

    # Build command arguments
    args = []

    if errors_only:
        args.append("-e")
    elif warnings_only:
        args.append("-w")
    elif exclude_debug:
        args.append("-x")
    elif level:
        args.extend(["-l", level])

    if category:
        args.extend(["-c", category])

    if peer:
        args.extend(["-p", peer])

    if test_only:
        args.append("-t")

    if time_pattern:
        args.extend(["-T", time_pattern])

    if context_lines:
        args.extend(["-C", str(context_lines)])

    if num_lines:
        args.extend(["-n", str(num_lines)])

    args.append(target_log)

    return run_filter(args)


@mcp.tool()
def find_errors(
    platform: Optional[str] = None,
    log_file: Optional[str] = None,
    context_lines: int = 3,
    category: Optional[str] = None
) -> str:
    """Find and analyze error messages with context.

    Args:
        platform: Platform log to analyze (posix, mactcp, or opentransport)
        log_file: Custom log file path
        context_lines: Number of context lines around errors (default: 3)
        category: Filter errors by category
    """
    target_log = log_file
    if not target_log:
        target_log = find_log_file(platform)

    if not target_log:
        return "Error: No log file found. Available logs: " + \
               ", ".join(f"{k}={v}" for k, v in DEFAULT_LOGS.items() if v.exists())

    if not Path(target_log).exists():
        return f"Error: Log file not found: {target_log}"

    args = ["-e", "-C", str(context_lines)]

    if category:
        args.extend(["-c", category])

    args.append(target_log)

    return run_filter(args)


@mcp.tool()
def analyze_peer(
    peer: str,
    platform: Optional[str] = None,
    log_file: Optional[str] = None,
    exclude_debug: bool = True
) -> str:
    """Analyze all activity for a specific peer.

    Args:
        peer: Peer name or IP address (REQUIRED)
        platform: Platform log to analyze (posix, mactcp, or opentransport)
        log_file: Custom log file path
        exclude_debug: Exclude DEBUG logs for clearer view (default: True)
    """
    target_log = log_file
    if not target_log:
        target_log = find_log_file(platform)

    if not target_log:
        return "Error: No log file found. Available logs: " + \
               ", ".join(f"{k}={v}" for k, v in DEFAULT_LOGS.items() if v.exists())

    if not Path(target_log).exists():
        return f"Error: Log file not found: {target_log}"

    args = ["-p", peer]

    if exclude_debug:
        args.append("-x")

    args.append(target_log)

    return run_filter(args)


@mcp.tool()
def analyze_test(
    platform: Optional[str] = None,
    log_file: Optional[str] = None,
    include_errors: bool = False
) -> str:
    """Analyze test execution results.

    Args:
        platform: Platform log to analyze (posix, mactcp, or opentransport)
        log_file: Custom log file path
        include_errors: Include error messages in test analysis
    """
    target_log = log_file
    if not target_log:
        target_log = find_log_file(platform)

    if not target_log:
        return "Error: No log file found. Available logs: " + \
               ", ".join(f"{k}={v}" for k, v in DEFAULT_LOGS.items() if v.exists())

    if not Path(target_log).exists():
        return f"Error: Log file not found: {target_log}"

    args = ["-t"]

    if include_errors:
        args.append("-w")

    args.append(target_log)

    return run_filter(args)


@mcp.tool()
def compare_platforms(
    filter_type: str = "summary"
) -> str:
    """Compare logs across different platforms.

    Args:
        filter_type: Type of comparison to perform (errors, warnings, test, or summary)
    """
    results = []

    for platform_name, log_path in DEFAULT_LOGS.items():
        if not log_path.exists():
            continue

        results.append(f"\n{'='*60}")
        results.append(f"Platform: {platform_name.upper()}")
        results.append(f"Log: {log_path.name}")
        results.append('='*60)

        if filter_type == "summary":
            output = run_filter(["-s", str(log_path)])
        elif filter_type == "errors":
            output = run_filter(["-e", str(log_path)])
        elif filter_type == "warnings":
            output = run_filter(["-w", str(log_path)])
        elif filter_type == "test":
            output = run_filter(["-t", str(log_path)])
        else:
            output = f"Unknown filter_type: {filter_type}"

        results.append(output)

    return "\n".join(results)


if __name__ == "__main__":
    mcp.run()
