# 🏷️ CSend Release Tags

---

## 🚀 [v1.2.0](https://github.com/matthewdeaves/csend/tree/v1.2.0) - Network Architecture Refactor
*Major networking improvements for Classic Mac*

<details>
<summary><strong>📋 What's New</strong></summary>

### 🔧 Key Improvements
- **Network Abstraction Layer**: Unified interface for all network operations
- **Dual-Stream TCP**: Separate streams for listening and sending (more reliable)
- **Message Queuing**: Better handling of multiple outgoing messages
- **Async UDP Operations**: Improved discovery performance

### 🛠️ Technical Changes
- Introduced `network_abstraction.c/.h` for implementation-agnostic networking
- Eliminated error-prone abort/restart patterns in TCP handling
- Added proper connection state management and resource cleanup
- Enhanced debugging and error reporting throughout network stack

</details>

**Impact**: Significantly more reliable networking, especially for Classic Mac. Eliminates the primary source of networking errors from previous versions.

---

## 🔧 [v0.1.1](https://github.com/matthewdeaves/csend/tree/v0.1.1) - Stability Improvements
*Classic Mac networking stability and system integration*

<details>
<summary><strong>📋 What's New</strong></summary>

### 🖥️ User Interface
- Added standard Apple Menu and File menu with Quit (⌘Q)
- Proper system shutdown/restart handling
- Larger window size (640x480 display)

### 🔌 Networking Fixes
- Fixed persistent shutdown errors (`-24 closeErr`)
- Resolved TCP re-listen issues (`duplicateSocketErr`)
- Added connection cooldown timers for reliable server operation
- Improved TCP send/receive cycle management

</details>

**Impact**: Much more stable Classic Mac experience with proper system integration.

---

## 📦 [v0.1.0](https://github.com/matthewdeaves/csend/tree/v0.1.0) - Major Refactoring
*Code organization and shared logging system*

<details>
<summary><strong>📋 What's New</strong></summary>

### 📝 Shared Logging System
- Unified logging across POSIX and Classic Mac platforms
- Platform-specific callbacks for timestamps and debug display
- New API: `log_init()`, `log_shutdown()`, `log_debug()`, `log_app_event()`

### 📁 File Reorganization
- Renamed files for better consistency (e.g., `discovery_logic.c` → `discovery.c`)
- Moved `main()` function to dedicated `posix/main.c`
- Improved platform-specific organization

### 🔄 Platform Updates
- **POSIX**: Better thread management and console output
- **Classic Mac**: Enhanced GUI integration and TextEdit logging

</details>

**Impact**: Better code organization, consistent logging, improved maintainability.

---

## 🖥️ [v0.0.12](https://github.com/matthewdeaves/csend/tree/v0.0.12) - GUI Bug Fixes
*Classic Mac user interface improvements*

<details>
<summary><strong>📋 What's New</strong></summary>

### ✅ Fixed Issues
- **Checkboxes**: "Show Debug" and "Broadcast" now update visually
- **Input Field**: Reliable border display and text clearing
- **Peer Selection**: Stable selection highlighting and functionality
- **Scrollbar**: Fixed crashes when dragging thumb

### 🎯 User Experience
- All GUI controls now work as expected
- Reliable peer-to-peer messaging
- Smooth scrolling in message area

</details>

**Impact**: Fully functional Classic Mac GUI, feature parity with POSIX version.

---

## ⚡ [v0.0.11](https://github.com/matthewdeaves/csend/tree/v0.0.11) - TCP Implementation
*Complete Classic Mac networking*

<details>
<summary><strong>📋 What's New</strong></summary>

### 🌐 Shared Discovery Logic
- Platform-independent UDP packet processing
- Callback system for platform-specific actions
- Classic Mac can now fully participate in peer discovery

### 📡 Complete TCP Implementation
- Full TCP message sending and receiving for Classic Mac
- Shared messaging logic with POSIX version
- Protocol magic number (`MSG_MAGIC_NUMBER`) for packet validation

### 🔄 Integration
- Classic Mac GUI integration with network operations
- Proper QUIT message handling on application exit

</details>

**Impact**: Classic Mac version now has full networking capabilities matching POSIX.

---

## 🔄 [v0.0.10](https://github.com/matthewdeaves/csend/tree/v0.0.10) - Shared Peer Management
*Cross-platform code sharing*

<details>
<summary><strong>📋 What's New</strong></summary>

### 🤝 Shared Logic
- Centralized peer management in `shared/peer_shared.c`
- Platform-independent peer operations (add, update, timeout)
- Consistent time handling across platforms

### 📊 Platform Wrappers
- **POSIX**: Thread-safe wrapper with mutex protection
- **Classic Mac**: Global peer list with GUI integration

### 📡 Classic Mac UDP
- Complete UDP discovery broadcasting implementation
- MacTCP-specific network operations
- Proper resource management

</details>

**Impact**: Reduced code duplication, consistent peer handling, UDP discovery for Classic Mac.

---

## 🔧 [v0.0.9](https://github.com/matthewdeaves/csend/tree/v0.0.9) - Cross-Platform Refactoring
*Better compatibility between platforms*

<details>
<summary><strong>📋 What's New</strong></summary>

### 🔄 Code Sharing
- Refactored peer management for better cross-platform compatibility
- Moved utility code to shared directory
- Enhanced dependency injection patterns

### 🛠️ Development Tools
- Added code duplication detection (`cpd_check.sh`)
- Enhanced Docker development environment
- Improved build system for Classic Mac

</details>

**Impact**: Better cross-platform compatibility, improved development workflow.

---

## 🖥️ [v0.0.8](https://github.com/matthewdeaves/csend/tree/v0.0.8) - Classic Mac GUI Shell
*Initial GUI implementation*

<details>
<summary><strong>📋 What's New</strong></summary>

### 🎨 GUI Foundation
- Initial Classic Mac GUI with Toolbox initialization
- Main event loop with `WaitNextEvent`
- Dialog window with resource definitions
- Basic mouse and update event handling

### 🔨 Build System
- Complete Makefile for Retro68 toolchain
- Resource compilation with `Rez`
- Organized build outputs

</details>

**Impact**: Foundation for Classic Mac GUI application.

---

## 📁 [v0.0.7](https://github.com/matthewdeaves/csend/tree/v0.0.7) - Project Structure Refactor
*Organized codebase for multiple platforms*

<details>
<summary><strong>📋 What's New</strong></summary>

- Created `posix/`, `classic_mac/`, and `shared/` directories
- Moved existing POSIX code to platform-specific directory
- Established shared module pattern with `protocol.c/h`
- Updated build system and development tools

</details>

**Impact**: Clean separation for multi-platform development.

---

## 🔄 [v0.0.6](https://github.com/matthewdeaves/csend/tree/v0.0.6) - Code Organization
*Improved modularity*

- Extracted utility functions into `utils.c`
- Created dedicated `signal_handler.c`
- Moved messaging code into `messaging.c`
- Added proper header files for all components

---

## 🛠️ [v0.0.5](https://github.com/matthewdeaves/csend/tree/v0.0.5) - Development Tools
*Testing and development infrastructure*

<details>
<summary><strong>📋 What's New</strong></summary>

### 🐳 Docker Testing
- Added Dockerfile and docker-compose.yml
- Script to launch 3 test containers (`p2p.sh`)
- Multi-peer testing environment

### 🔧 Code Organization
- Separated discovery logic into dedicated files
- Better project structure

</details>

**Usage**: `./p2p.sh start` to launch test environment

---

## 🖥️ [v0.0.4](https://github.com/matthewdeaves/csend/tree/v0.0.4) - Terminal UI Refactor
*Separated user interface code*

- Moved terminal UI to `ui_terminal.c/h`
- Added Makefile for easier building
- Added `codemerge.sh` tool for LLM analysis

**Build**: `make` then `./p2p_chat`

---

## ⚡ [v0.0.3](https://github.com/matthewdeaves/csend/tree/v0.0.3) - P2P Implementation
*Full peer-to-peer communication*

<details>
<summary><strong>📋 What's New</strong></summary>

### 🌐 Networking
- UDP peer discovery with broadcasting
- TCP direct and broadcast messaging
- Structured message protocol

### 🧵 Threading
- Discovery thread for peer finding
- Listener thread for incoming messages
- User input thread for commands
- Thread-safe peer list with mutexes

### 📡 Protocol
Simple message format: `TYPE|SENDER|CONTENT`
</details>

**Build**: `gcc -o p2p_chat peer.c network.c protocol.c -lpthread`

---

## 🔄 [v0.0.2](https://github.com/matthewdeaves/csend/tree/v0.0.2) - Continuous Server
*Persistent server operation*

- Added signal handling (SIGINT/SIGTERM)
- Non-blocking accept with 1-second timeout
- Graceful shutdown handling
- Continuous operation until terminated

---

## 🚀 [v0.0.1](https://github.com/matthewdeaves/csend/tree/v0.0.1) - Initial Implementation
*Basic client-server demo*

Simple socket-based client and server on localhost. Server accepts one connection, exchanges messages, then terminates. Foundation for peer-to-peer development.

**Build**: `gcc client.c -o client && gcc server.c -o server`

---

<div align="center">

### 📖 Getting Started

| Want to... | Go to |
|------------|-------|
| **Build the project** | [README.md](README.md) |
| **Understand the code** | [CLAUDE.md](CLAUDE.md) |
| **Understand the logs** | [LOGGING.md](LOGGING.md) |
| **See latest features** | [v1.2.0](#-v120---network-architecture-refactor) |

</div>