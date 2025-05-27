# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# CSend Project Information

CSend is a cross-platform peer-to-peer terminal chat application written in C, supporting both POSIX systems (Linux/macOS) and Classic Macintosh (System 7.x).

## Architecture Overview

The project uses a **shared core** design with platform-specific implementations:

- **`shared/`** - Platform-independent protocol handling, peer management, discovery, and messaging logic
- **`posix/`** - POSIX-specific implementation (multi-threaded, terminal UI)
- **`classic_mac/`** - Classic Mac implementation (event-driven GUI, MacTCP networking)

### Key Architectural Patterns

1. **Protocol**: Custom format `MSG_MAGIC_NUMBER|TYPE|SENDER@IP|CONTENT` with message types: DISCOVERY, TEXT, QUIT
2. **Networking**: UDP for peer discovery (broadcast), TCP for direct messaging
3. **Shared Logic with Platform Callbacks**: Core logic in `shared/` uses callback interfaces for platform-specific operations
4. **POSIX**: Multi-threaded design with separate threads for input, listening, and discovery
5. **Classic Mac**: Single-threaded event loop with asynchronous network operations, dual TCP streams (listen + send)

## Build Commands

### POSIX Build
```bash
make clean
make
# Output: build/posix/csend_posix
```

### Classic Mac Build
```bash
make -f Makefile.retro68
# Output: build/classic_mac/csend-mac.{APPL,bin,dsk}
# Requires Retro68 cross-compiler (use setup_retro68.sh)
```

### Running the Application

**POSIX**:
```bash
./build/posix/csend_posix <username>
# Commands: /list, /send <peer_num> <msg>, /broadcast <msg>, /debug, /quit, /help
```

**Docker Testing** (multiple instances):
```bash
./docker.sh start   # Start 3 containers
./docker.sh exec 0  # Connect to container
./docker.sh stop    # Stop all containers
```

**Machine Mode** (JSON-based programmatic interaction):
```bash
./run_machine_mode.sh
# Or: ./build/posix/csend_posix <username> --machine-mode
```

## Development Tools

### Code Formatting
```bash
./format_code.sh  # Uses astyle with K&R style, 4-space indentation
```

### Duplicate Code Detection
```bash
./cpd_check.sh    # Uses PMD's CPD tool (requires Java)
```

### Complexity Analysis
```bash
# Install: pip install lizard
./complexity_check.sh         # Basic analysis with all functions
./complexity_check.sh warnings # Show only complex functions
./complexity_check.sh detailed # Generate HTML and CSV reports
# Thresholds: CCN=10, Length=60 lines, Parameters=5
```

### Dead Code Detection
```bash
./deadcode_check.sh       # Full analysis (all phases)
./deadcode_check.sh warnings # Only compiler warnings
./deadcode_check.sh symbols  # Symbol analysis
./deadcode_check.sh sections # Section-based analysis

# Platform-specific analysis:
./deadcode_check.sh -p posix     # POSIX only
./deadcode_check.sh -p classic   # Classic Mac only
./deadcode_check.sh -p all       # All platforms (default)

# Uses GCC flags: -Wunused-*, -ffunction-sections, --gc-sections
# Optional: Install cppcheck for additional analysis
```

### Log Filtering
```bash
./filter_logs.sh <log_file> [categories]  # Filter logs by category
# Categories: NETWORK, DISCOVERY, MESSAGING, UI, APP_EVENT, ERROR, DEBUG
```

## Testing Approaches

- **Unit Testing**: No formal test framework currently in use
- **Integration Testing**: Use Docker setup for multi-peer testing
- **Machine Mode Testing**: `test_machine_mode.py` for automated testing
- **Logging**: Enable debug mode (`/debug` command) and check log files:
  - POSIX: `csend_posix.log`
  - Classic Mac: `csend_classic_mac.log`

## Important Implementation Notes

1. **Thread Safety (POSIX)**: Peer list access protected by `pthread_mutex_t` in `app_state_t`
2. **Network Abstraction (Classic Mac)**: All network ops go through `network_abstraction.h` interface for future OpenTransport support
3. **Message Queue (Classic Mac)**: Broadcast messages queued when send stream busy
4. **Timeout Handling**: Peers pruned after `PEER_TIMEOUT` (30 seconds) of inactivity
5. **Protocol Endianness**: Magic number uses network byte order (`htonl`/`ntohl`)
6. **Logging System**: Uses categorized logging (see LOGGING.md) with levels ERROR, WARNING, INFO, DEBUG

## Key Files to Understand

- **Protocol**: `shared/protocol.c` - Message format and parsing
- **Peer Management**: `shared/peer.c`, `posix/peer.c`, `classic_mac/peer.c`
- **Command Handlers**: `posix/commands.c` - Application command implementations (UI-agnostic)
- **POSIX Main Loop**: `posix/main.c` - Thread management and initialization
- **Classic Mac Event Loop**: `classic_mac/main.c` - WaitNextEvent handling
- **Network Abstraction**: `classic_mac/network_abstraction.h` - Network interface design
- **Machine Mode UI**: `posix/ui_terminal_machine.c` - JSON-based interface implementation
- **Logging**: `shared/logging.c` - Centralized logging with categories and levels

## Code Quality Commands

**IMPORTANT**: When making code changes, run these commands to ensure code quality:

```bash
# Format code (required before commits)
./format_code.sh

# Check for code duplication
./cpd_check.sh

# Check complexity warnings
./complexity_check.sh warnings

# Check for dead code
./deadcode_check.sh warnings
```

## CI/CD Integration

For CI/CD pipelines, add these checks:
```bash
# Code quality checks
./format_code.sh && git diff --exit-code  # Verify formatting
./cpd_check.sh                             # Duplicate detection
./complexity_check.sh warnings             # Complexity warnings
./deadcode_check.sh warnings               # Dead code warnings

# Platform-specific builds
make clean && make                         # POSIX build
make -f Makefile.retro68                   # Classic Mac (requires Retro68)
```