# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# CSend Project Information

CSend is a cross-platform peer-to-peer terminal chat application written in C, supporting both POSIX systems (Linux/macOS) and Classic Macintosh (System 7.x). The project demonstrates network programming across different computing eras, featuring modern multi-threaded architecture alongside single-threaded event-driven GUI applications.

## Architecture Overview

The project uses a **shared core** design with platform-specific implementations:

- **`shared/`** - Platform-independent protocol handling, peer management, discovery, and messaging logic
- **`posix/`** - POSIX-specific implementation (multi-threaded, terminal UI)
- **`classic_mac/`** - Classic Mac implementation (event-driven GUI, MacTCP networking)
- **`tools/`** - Python client libraries and automation scripts
- **`scripts/`** - Shell scripts for building, testing, and code quality
- **`docs/`** - Documentation files and project images
- **`resources/`** - External resources (Apple documentation, MPW interfaces)
- **`.finf/`** - SheepShaver metadata folders (preserve Mac resource/data forks)

### Key Architectural Patterns

1. **Protocol**: Custom format `MSG_MAGIC_NUMBER|TYPE|SENDER@IP|CONTENT` with message types: DISCOVERY, DISCOVERY_RESPONSE, TEXT, QUIT
2. **Networking**: UDP for peer discovery (broadcast on port 8181), TCP for direct messaging (port 8080)
3. **Shared Logic with Platform Callbacks**: Core logic in `shared/` uses callback interfaces for platform-specific operations
4. **POSIX**: Multi-threaded design with separate threads for input, listening, and discovery
5. **Classic Mac**: Single-threaded event loop with asynchronous network operations, dual TCP streams (listen + send)
6. **Machine Mode**: JSON-based API for programmatic interaction with correlation IDs and event streaming

## Build Commands

### POSIX Build
```bash
make clean
make
# Output: build/posix/csend_posix
```

### Classic Mac MacTCP Build
```bash
make -f Makefile.retro68.mactcp
# Output: build/classic_mac/csend-mac.{APPL,bin,dsk}
# Requires Retro68 cross-compiler (use scripts/setup_retro68.sh)
```

### Running the Application

**POSIX**:
```bash
./build/posix/csend_posix <username>
# Commands: /list, /send <peer_num> <msg>, /broadcast <msg>, /debug, /quit, /help
```

**Docker Testing** (multiple instances):
```bash
scripts/docker.sh start   # Start 3 containers
scripts/docker.sh exec 0  # Connect to container
scripts/docker.sh stop    # Stop all containers
```

**Machine Mode** (JSON-based programmatic interaction):
```bash
scripts/run_machine_mode.sh
# Or: ./build/posix/csend_posix <username> --machine-mode
```

**AI Chatbot Integration**:
```bash
# Set up Anthropic API key
export ANTHROPIC_API_KEY="your-api-key-here"

# Run Claude chatbot
scripts/run_machine_mode.sh --chatbot
```

## Development Tools

### Code Formatting
```bash
scripts/format_code.sh  # Uses astyle with K&R style, 4-space indentation
```

### Duplicate Code Detection
```bash
scripts/cpd_check.sh    # Uses PMD's CPD tool (requires Java)
```

### Complexity Analysis
```bash
# Install: pip install lizard
scripts/complexity_check.sh         # Basic analysis with all functions
scripts/complexity_check.sh warnings # Show only complex functions
scripts/complexity_check.sh detailed # Generate HTML and CSV reports
# Thresholds: CCN=10, Length=60 lines, Parameters=5
```

### Dead Code Detection
```bash
scripts/deadcode_check.sh       # Full analysis (all phases)
scripts/deadcode_check.sh warnings # Only compiler warnings
scripts/deadcode_check.sh symbols  # Symbol analysis
scripts/deadcode_check.sh sections # Section-based analysis

# Platform-specific analysis:
scripts/deadcode_check.sh -p posix     # POSIX only
scripts/deadcode_check.sh -p classic   # Classic Mac only
scripts/deadcode_check.sh -p all       # All platforms (default)

# Uses GCC flags: -Wunused-*, -ffunction-sections, --gc-sections
# Optional: Install cppcheck for additional analysis
```

### Log Analysis and Filtering
```bash
# Quick summary of log file
scripts/filter_logs.sh -s csend_posix.log

# Filter by level
scripts/filter_logs.sh -e csend_posix.log              # Errors only
scripts/filter_logs.sh -w csend_posix.log              # Warnings + Errors
scripts/filter_logs.sh -x csend_posix.log              # Exclude DEBUG

# Filter by category
scripts/filter_logs.sh -c NETWORKING csend_posix.log   # Categories: NETWORKING, DISCOVERY, PEER_MGMT, MESSAGING, etc.

# Filter by peer
scripts/filter_logs.sh -p MacTCP csend_posix.log       # Filter by peer name/IP

# Test analysis
scripts/filter_logs.sh -t csend_posix.log              # Show only test messages

# Advanced filtering
scripts/filter_logs.sh -C 3 -e csend_posix.log         # Errors with 3 lines of context
scripts/filter_logs.sh -T "16:44:2" csend_posix.log    # Filter by timestamp
scripts/filter_logs.sh -n -50 csend_posix.log          # Last 50 lines

# See scripts/FILTER_LOGS_EXAMPLES.md for comprehensive examples
```

**MCP Integration**: The log filtering tool is available as a Model Context Protocol (MCP) server for Claude. This provides structured log analysis during debugging sessions. See `tools/MCP_SETUP.md` for setup instructions. The MCP is project-specific and validates it's running in the CSend directory.

## Testing Approaches

- **Automated Test Feature**: Built-in test command that exercises the application's messaging code:
  - Classic Mac (MacTCP & OpenTransport): Select **"Perform Test"** from File menu
  - POSIX: Use `/test` command
  - Test sequence: 2 rounds of broadcasts→direct messages (4 second delays)
  - Generates clear START/END markers in log files
  - **CRITICAL**: Test feature reuses existing application messaging functions (not duplicate code paths)
  - Implementation: `shared/test.c` (core logic) + platform adapters (`*/test.c`)
  - See `docs/PERFORM_TEST_IMPLEMENTATION_PLAN.md` for architecture details
- **Unit Testing**: No formal test framework currently in use (contributions welcome)
- **Integration Testing**: Use Docker setup for multi-peer testing scenarios
- **Machine Mode Testing**: `tools/test_machine_mode.py` for automated JSON API testing
- **AI Integration Testing**: Python client library (`tools/csend_client.py`) for chatbot testing
- **Logging**: Enable debug mode (`/debug` command) and check log files:
  - POSIX: `csend_posix.log`
  - Classic Mac MacTCP: `csend_mac.log`
  - Classic Mac OpenTransport: `csend_classic_mac_ot_ppc.log`
- **Cross-Platform Testing**: Build verification on both POSIX and Classic Mac (via Retro68)

## Important Implementation Notes

1. **Thread Safety (POSIX)**: Peer list access protected by `pthread_mutex_t` in `app_state_t`
2. **Network Abstraction (Classic Mac)**: All network ops go through `network_abstraction.h` interface for future OpenTransport support
3. **Message Queue (Classic Mac)**: Broadcast messages queued when send stream busy
4. **Timeout Handling**: Peers pruned after `PEER_TIMEOUT` (30 seconds) of inactivity
5. **Protocol Endianness**: Magic number uses network byte order (`htonl`/`ntohl`)
6. **Logging System**: Uses categorized logging (see docs/LOGGING.md) with levels ERROR, WARNING, INFO, DEBUG
7. **Machine Mode Threading**: JSON output is thread-safe with proper synchronization
8. **AI Integration**: Claude Haiku chatbot with configurable behavior and rate limiting
9. **Cross-Platform Time**: `time()` (POSIX) vs `TickCount()` (Classic Mac) abstraction
10. **Resource Fork Preservation**: `.finf` folders maintain Mac resource/data fork metadata for ResEdit-modified files

## Key Files to Understand

- **Protocol**: `shared/protocol.c` - Message format and parsing
- **Peer Management**: `shared/peer.c`, `posix/peer.c`, `classic_mac/peer.c`
- **Command Handlers**: `posix/commands.c` - Application command implementations (UI-agnostic)
- **POSIX Main Loop**: `posix/main.c` - Thread management and initialization
- **Classic Mac Event Loop**: `classic_mac/main.c` - WaitNextEvent handling
- **Network Abstraction**: `classic_mac/network_abstraction.h` - Network interface design
- **Machine Mode UI**: `posix/ui_terminal_machine.c` - JSON-based interface implementation
- **UI Factory**: `posix/ui_factory.c` - Strategy pattern for UI creation
- **Python Integration**: `tools/csend_client.py`, `tools/csend_chatbot.py` - AI chatbot and automation
- **Logging**: `shared/logging.c` - Centralized logging with categories and levels

## Code Quality Commands

**IMPORTANT**: When making code changes, run these commands to ensure code quality:

```bash
# Format code (required before commits)
scripts/format_code.sh

# Check for code duplication
scripts/cpd_check.sh

# Check complexity warnings
scripts/complexity_check.sh warnings

# Check for dead code
scripts/deadcode_check.sh warnings
```

## CI/CD Integration

For CI/CD pipelines, add these checks:
```bash
# Code quality checks
scripts/format_code.sh && git diff --exit-code  # Verify formatting
scripts/cpd_check.sh                             # Duplicate detection
scripts/complexity_check.sh warnings             # Complexity warnings
scripts/deadcode_check.sh warnings               # Dead code warnings

# Platform-specific builds
make clean && make                         # POSIX build
make -f Makefile.retro68.mactcp            # Classic Mac MacTCP (requires Retro68)
make -f Makefile.retro68.ot.ppc all         # Classic Mac OpenTransport PowerPC
```