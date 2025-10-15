#!/usr/bin/env python3
"""
CSend Documentation MCP Server

This MCP server provides access to Classic Macintosh programming documentation
stored in the resources/Books folder. It allows searching and extracting
information from Inside Macintosh volumes, MacTCP documentation, and OpenTransport
references without loading entire documents into context.

IMPORTANT: This MCP is ONLY for use with the CSend project. It validates
the project structure and will refuse to run in other directories.

Usage:
    python3 csend_docs_mcp.py
"""

import sys
import re
from pathlib import Path
from typing import Optional, List

# Add MCP SDK to path if available
try:
    from mcp.server.fastmcp import FastMCP
except ImportError:
    print("Error: MCP SDK not installed. Install with: pip install mcp", file=sys.stderr)
    sys.exit(1)

# Get project root directory
SCRIPT_DIR = Path(__file__).parent.absolute()
PROJECT_ROOT = SCRIPT_DIR.parent
BOOKS_DIR = PROJECT_ROOT / "resources" / "Books"

# Validate this is the CSend project
def validate_csend_project() -> bool:
    """
    Validate that we're running in the CSend project directory.
    Checks for key files and directories that uniquely identify CSend.
    """
    required_markers = [
        PROJECT_ROOT / "CLAUDE.md",
        PROJECT_ROOT / "shared" / "protocol.c",
        PROJECT_ROOT / "posix" / "main.c",
        BOOKS_DIR,  # Books directory must exist
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
        "Required: A directory containing CSend project files (CLAUDE.md, shared/protocol.c, resources/Books/, etc.)",
        file=sys.stderr
    )
    sys.exit(1)

# Available documentation files
AVAILABLE_DOCS = {
    "inside-macintosh-text": BOOKS_DIR / "Inside Macintosh - Text.txt",
    "inside-macintosh-1-2-3": BOOKS_DIR / "Inside Macintosh Volume I, II, III - 1985.txt",
    "inside-macintosh-4": BOOKS_DIR / "Inside_Macintosh_Volume_IV_1986.txt",
    "inside-macintosh-5": BOOKS_DIR / "Inside_Macintosh_Volume_V_1986.txt",
    "inside-macintosh-6": BOOKS_DIR / "Inside_Macintosh_Volume_VI_1991.txt",
    "mactcp-guide": BOOKS_DIR / "MacTCP_Programmers_Guide_1989.txt",
    "mactcp-programming": BOOKS_DIR / "MacTCP_programming.txt",
    "opentransport": BOOKS_DIR / "NetworkingOpenTransport.txt",
}

# Initialize FastMCP server
mcp = FastMCP("csend-docs")


def search_in_file(file_path: Path, pattern: str, context_lines: int = 3, case_sensitive: bool = False, max_results: int = 10) -> str:
    """Search for a pattern in a file and return matches with context."""
    if not file_path.exists():
        return f"Error: File not found: {file_path}"

    try:
        content = file_path.read_text(encoding='utf-8', errors='ignore')
        lines = content.split('\n')

        # Compile regex pattern
        flags = 0 if case_sensitive else re.IGNORECASE
        try:
            regex = re.compile(pattern, flags)
        except re.error as e:
            return f"Error: Invalid regex pattern: {e}"

        results = []
        matches_found = 0

        for i, line in enumerate(lines):
            if regex.search(line):
                matches_found += 1
                if matches_found > max_results:
                    break

                # Get context
                start = max(0, i - context_lines)
                end = min(len(lines), i + context_lines + 1)

                context = []
                for j in range(start, end):
                    prefix = ">>> " if j == i else "    "
                    context.append(f"{prefix}Line {j+1}: {lines[j]}")

                results.append("\n".join(context))
                results.append("-" * 60)

        if not results:
            return f"No matches found for pattern: {pattern}"

        header = f"Found {matches_found} match(es) in {file_path.name}"
        if matches_found > max_results:
            header += f" (showing first {max_results})"

        return header + "\n" + "=" * 60 + "\n\n" + "\n".join(results)

    except Exception as e:
        return f"Error reading file: {e}"


def extract_section(file_path: Path, section_name: str, lines_to_read: int = 100) -> str:
    """Extract a section from a documentation file."""
    if not file_path.exists():
        return f"Error: File not found: {file_path}"

    try:
        content = file_path.read_text(encoding='utf-8', errors='ignore')
        lines = content.split('\n')

        # Find section (case-insensitive)
        section_pattern = re.compile(re.escape(section_name), re.IGNORECASE)

        for i, line in enumerate(lines):
            if section_pattern.search(line):
                # Found section, extract lines
                start = i
                end = min(len(lines), i + lines_to_read)

                extracted = lines[start:end]
                result = f"Section found at line {i+1} in {file_path.name}\n"
                result += "=" * 60 + "\n\n"
                result += "\n".join(extracted)

                if end < len(lines):
                    result += f"\n\n[... {len(lines) - end} more lines in file ...]"

                return result

        return f"Section '{section_name}' not found in {file_path.name}"

    except Exception as e:
        return f"Error reading file: {e}"


@mcp.tool()
def list_documentation() -> str:
    """List all available documentation files with their sizes and topics."""
    result = ["Available Classic Mac Documentation:", "=" * 60, ""]

    for doc_id, doc_path in AVAILABLE_DOCS.items():
        if doc_path.exists():
            size_mb = doc_path.stat().st_size / (1024 * 1024)
            result.append(f"• {doc_id}")
            result.append(f"  File: {doc_path.name}")
            result.append(f"  Size: {size_mb:.2f} MB")
            result.append("")
        else:
            result.append(f"• {doc_id} (NOT FOUND)")
            result.append("")

    result.append("Usage: Use search_documentation() or get_section() to query specific topics.")

    return "\n".join(result)


@mcp.tool()
def search_documentation(
    query: str,
    doc_filter: Optional[str] = None,
    context_lines: int = 3,
    case_sensitive: bool = False,
    max_results: int = 10
) -> str:
    """Search for a topic, function, or concept across documentation files.

    Args:
        query: Search pattern (supports regex)
        doc_filter: Filter to specific docs (e.g., 'mactcp', 'opentransport', 'inside-macintosh')
        context_lines: Number of context lines around matches (default: 3)
        case_sensitive: Perform case-sensitive search (default: False)
        max_results: Maximum number of results to return (default: 10)
    """
    results = []

    # Determine which docs to search
    if doc_filter:
        docs_to_search = {k: v for k, v in AVAILABLE_DOCS.items() if doc_filter.lower() in k.lower()}
        if not docs_to_search:
            return f"No documentation matches filter: {doc_filter}\nAvailable docs: {', '.join(AVAILABLE_DOCS.keys())}"
    else:
        docs_to_search = AVAILABLE_DOCS

    for doc_id, doc_path in docs_to_search.items():
        if not doc_path.exists():
            continue

        result = search_in_file(doc_path, query, context_lines, case_sensitive, max_results)

        if "No matches found" not in result:
            results.append(f"\n{'='*60}")
            results.append(f"Results from: {doc_id}")
            results.append('='*60)
            results.append(result)

    if not results:
        return f"No matches found for query: {query}"

    return "\n".join(results)


@mcp.tool()
def get_section(
    section_name: str,
    doc_id: str,
    lines_to_read: int = 100
) -> str:
    """Extract a specific section from a documentation file.

    Args:
        section_name: Name of the section to extract (e.g., 'TCP Streams', 'Event Manager')
        doc_id: Documentation ID to search in (use list_documentation() to see available IDs)
        lines_to_read: Number of lines to extract from the section (default: 100)
    """
    if doc_id not in AVAILABLE_DOCS:
        return f"Unknown documentation ID: {doc_id}\nAvailable IDs: {', '.join(AVAILABLE_DOCS.keys())}"

    doc_path = AVAILABLE_DOCS[doc_id]

    if not doc_path.exists():
        return f"Documentation file not found: {doc_path}"

    return extract_section(doc_path, section_name, lines_to_read)


@mcp.tool()
def search_mactcp(
    query: str,
    context_lines: int = 5,
    max_results: int = 10
) -> str:
    """Search specifically in MacTCP documentation (both guides).

    Args:
        query: Search pattern (supports regex)
        context_lines: Number of context lines around matches (default: 5)
        max_results: Maximum number of results to return (default: 10)
    """
    return search_documentation(query, doc_filter="mactcp", context_lines=context_lines, max_results=max_results)


@mcp.tool()
def search_opentransport(
    query: str,
    context_lines: int = 5,
    max_results: int = 10
) -> str:
    """Search specifically in OpenTransport documentation.

    Args:
        query: Search pattern (supports regex)
        context_lines: Number of context lines around matches (default: 5)
        max_results: Maximum number of results to return (default: 10)
    """
    return search_documentation(query, doc_filter="opentransport", context_lines=context_lines, max_results=max_results)


@mcp.tool()
def search_inside_macintosh(
    query: str,
    volume: Optional[str] = None,
    context_lines: int = 3,
    max_results: int = 10
) -> str:
    """Search specifically in Inside Macintosh volumes.

    Args:
        query: Search pattern (supports regex)
        volume: Specific volume to search (e.g., '6' for Volume VI, or leave blank for all)
        context_lines: Number of context lines around matches (default: 3)
        max_results: Maximum number of results to return (default: 10)
    """
    doc_filter = "inside-macintosh"
    if volume:
        doc_filter = f"inside-macintosh-{volume}"

    return search_documentation(query, doc_filter=doc_filter, context_lines=context_lines, max_results=max_results)


if __name__ == "__main__":
    mcp.run()
