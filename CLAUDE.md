# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# CSend Project Information

CSend is a cross-platform peer-to-peer terminal chat application written in C, supporting both POSIX systems (Linux/macOS) and Classic Macintosh (System 7.x). The project demonstrates network programming across different computing eras, featuring modern multi-threaded architecture alongside single-threaded event-driven GUI applications.

## Architecture Overview

The project uses a **shared core** design with platform-specific implementations:

- **`shared/`** - Platform-independent protocol handling, peer management, discovery, and messaging logic
- **`posix/`** - POSIX-specific implementation (multi-threaded, terminal UI)
- **`classic_mac/`** - Classic Mac implementation (event-driven GUI, MacTCP networking)

### Key Architectural Patterns

1. **Protocol**: Custom format `MSG_MAGIC_NUMBER|TYPE|SENDER@IP|CONTENT` with message types: DISCOVERY, DISCOVERY_RESPONSE, TEXT, QUIT
2. **Networking**: UDP for peer discovery (broadcast on port 2556), TCP for direct messaging (port 2555)
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

**AI Chatbot Integration**:
```bash
# Set up Anthropic API key
export ANTHROPIC_API_KEY="your-api-key-here"

# Run Claude chatbot
./run_machine_mode.sh --chatbot
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

- **Unit Testing**: No formal test framework currently in use (contributions welcome)
- **Integration Testing**: Use Docker setup for multi-peer testing scenarios
- **Machine Mode Testing**: `test_machine_mode.py` for automated JSON API testing
- **AI Integration Testing**: Python client library (`csend_client.py`) for chatbot testing
- **Logging**: Enable debug mode (`/debug` command) and check log files:
  - POSIX: `csend_posix.log`
  - Classic Mac: `csend_classic_mac.log`
- **Cross-Platform Testing**: Build verification on both POSIX and Classic Mac (via Retro68)

## Important Implementation Notes

1. **Thread Safety (POSIX)**: Peer list access protected by `pthread_mutex_t` in `app_state_t`
2. **Network Abstraction (Classic Mac)**: All network ops go through `network_abstraction.h` interface for future OpenTransport support
3. **Message Queue (Classic Mac)**: Broadcast messages queued when send stream busy
4. **Timeout Handling**: Peers pruned after `PEER_TIMEOUT` (30 seconds) of inactivity
5. **Protocol Endianness**: Magic number uses network byte order (`htonl`/`ntohl`)
6. **Logging System**: Uses categorized logging (see LOGGING.md) with levels ERROR, WARNING, INFO, DEBUG
7. **Machine Mode Threading**: JSON output is thread-safe with proper synchronization
8. **AI Integration**: Claude Haiku chatbot with configurable behavior and rate limiting
9. **Cross-Platform Time**: `time()` (POSIX) vs `TickCount()` (Classic Mac) abstraction

## Key Files to Understand

- **Protocol**: `shared/protocol.c` - Message format and parsing
- **Peer Management**: `shared/peer.c`, `posix/peer.c`, `classic_mac/peer.c`
- **Command Handlers**: `posix/commands.c` - Application command implementations (UI-agnostic)
- **POSIX Main Loop**: `posix/main.c` - Thread management and initialization
- **Classic Mac Event Loop**: `classic_mac/main.c` - WaitNextEvent handling
- **Network Abstraction**: `classic_mac/network_abstraction.h` - Network interface design
- **MacTCP Implementation**: `classic_mac/mactcp_impl.c` - MacTCP-specific networking
- **OpenTransport Implementation**: `classic_mac/opentransport_impl.c` - OpenTransport networking framework
- **Machine Mode UI**: `posix/ui_terminal_machine.c` - JSON-based interface implementation
- **UI Factory**: `posix/ui_factory.c` - Strategy pattern for UI creation
- **Python Integration**: `csend_client.py`, `csend_chatbot.py` - AI chatbot and automation
- **Logging**: `shared/logging.c` - Centralized logging with categories and levels

## Retro68 Development Resources

**IMPORTANT**: If the `Retro68Reference/` directory is present, it contains the complete Retro68 Interfaces&Libraries collection, providing comprehensive Classic Mac development resources including full OpenTransport support. This directory should be:
- Used for reference when implementing Classic Mac features  
- Ignored by git (development reference only)
- Consulted for available headers, libraries, and documentation

**Retro68 OpenTransport Support**: Retro68 includes comprehensive OpenTransport support in the toolchain:
- Headers: `OpenTransport.h`, `OpenTptInternet.h`, `OpenTptClient.h`, etc.
- Libraries: `OpenTransport.o`, `OpenTptInet.o`, `OpenTptUtils.o` (68K), plus PowerPC versions
- **CSend always builds with BOTH MacTCP and OpenTransport support**
- **Runtime Detection**: At startup, CSend detects available networking and prefers OpenTransport when available
- **Automatic Fallback**: Falls back to MacTCP on older systems without OpenTransport

**Retro68Reference/ Directory**: Contains reference copies of Retro68 interfaces for development assistance - used by Claude Code for implementing Classic Mac features, not part of the build process.

## Classic Mac Programming Documentation

**CRITICAL**: When working with Classic Mac networking implementations, ALWAYS consult the authoritative Apple documentation available in the `Books/` directory. These books contain the definitive specifications, examples, and best practices for Classic Mac programming.

### Primary References for Network Programming

**OpenTransport Programming** (`Books/NetworkingOpenTransport.txt`):
- **Inside Macintosh: Networking With Open Transport** (Version 1.3, November 1997)
- Comprehensive guide to OpenTransport programming on Classic Mac
- Use for: OpenTransport initialization, endpoint management, async operations, TCP/IP services
- Covers: Provider architecture, XTI compatibility, option management, error handling
- Essential for implementing `classic_mac/opentransport_impl.c` functionality

**MacTCP Programming** (`Books/MacTCP_Programmers_Guide_1989.txt` and `Books/MacTCP_programming.txt`):
- Official MacTCP programmer's documentation
- Use for: MacTCP driver interface, parameter blocks, async operations, DNS resolution
- Covers: TCPiopb/UDPiopb structures, callback procedures, stream management
- Essential for maintaining `classic_mac/mactcp_impl.c` implementation

**Inside Macintosh Volumes** (Complete collection available):
- `Books/Inside Macintosh Volume I, II, III - 1985.txt` - Toolbox fundamentals
- `Books/Inside_Macintosh_Volume_IV_1986.txt` - Additional Toolbox managers
- `Books/Inside_Macintosh_Volume_V_1986.txt` - More Toolbox extensions
- `Books/Inside_Macintosh_Volume_VI_1991.txt` - System 7 features and updates
- `Books/Inside Macintosh - Text.txt` - Comprehensive text reference

### Documentation Usage Guidelines

**When implementing network features:**
1. **ALWAYS check the appropriate book first** before writing network code
2. **Use exact parameter block structures** as documented in the books
3. **Follow Apple's recommended async patterns** for non-blocking operations
4. **Implement proper error handling** using documented error codes
5. **Use correct calling conventions** (Pascal vs C) as specified

**When debugging network issues:**
1. **Consult error code appendices** in the networking books
2. **Check parameter validation** against documented requirements
3. **Verify async operation patterns** match Apple's examples
4. **Review memory management** for parameter blocks and buffers

**When adding new networking functionality:**
1. **Research the feature** in OpenTransport book first, then MacTCP if needed
2. **Follow established patterns** from existing implementations
3. **Implement both sync and async variants** when available
4. **Add comprehensive error translation** between OS errors and network abstraction

**Example Research Workflow:**
```
1. Identify networking requirement (e.g., "implement UDP broadcast")
2. Search Books/NetworkingOpenTransport.txt for OpenTransport approach
3. Search Books/MacTCP_programming.txt for MacTCP equivalent
4. Compare approaches and identify common abstractions
5. Implement in network_abstraction.h interface
6. Create implementation-specific code in mactcp_impl.c and opentransport_impl.c
```

**Code Comments Reference Style:**
When implementing network functions, include references to the documentation:
```c
/* Implements TCP passive open as documented in MacTCP Programmer's Guide */
/* See OpenTransport book Chapter 5 for equivalent OT implementation */
```

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