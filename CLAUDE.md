# CLAUDE.md

# CSend Project Information

CSend is a cross-platform peer-to-peer chat application written in C, supporting both POSIX systems (Linux/macOS) and Classic Macintosh (System 7.x). Built on the PeerTalk SDK for all networking.

## Architecture Overview

The project is a thin UI layer on top of the **PeerTalk SDK** (`~/peertalk`), which handles all networking (discovery, connections, messaging) across POSIX, MacTCP, and OpenTransport platforms.

- **`posix/`** - POSIX terminal UI (interactive + machine mode), 2-thread model
- **`classic_mac/`** - Classic Mac GUI (Dialog Manager, single-threaded event loop)
- **`shared/`** - Common definitions, shared test framework, Classic Mac UI components
- **`MPW_resources/`** - ResEdit binary resources (MUST preserve `.finf/` and `.rsrc/` metadata)

### Key Patterns

1. **PeerTalk SDK**: 22-function C89 API. `PT_Init()`, `PT_Poll()`, `PT_Send()`, `PT_Broadcast()`, callbacks for events
2. **POSIX Threading**: Main thread (PT_Poll + command queue processing) + input thread (stdin). PeerTalk is NOT thread-safe — input thread pushes commands to SPSC ring buffer, main thread executes them
3. **Classic Mac**: Single-threaded. `PT_Poll()` called from `HandleIdleTasks()` in main event loop. Safe to call PT_Send directly
4. **Message Type**: `#define MSG_CHAT 1` registered with `PT_RegisterMessage(ctx, MSG_CHAT, PT_RELIABLE)`
5. **Auto-connect**: `on_peer_discovered` callback calls `PT_Connect()` automatically
6. **Logging**: Uses clog (`~/Desktop/clog`) — `CLOG_INFO()`, `CLOG_ERR()`, `CLOG_WARN()`, `CLOG_DEBUG()`
7. **UI Strategy Pattern**: `ui_operations_t` function pointer struct with `UI_CALL()` macro, two implementations (interactive/machine)
8. **Peer Indexing**: Connected peers filtered from all peers. `PT_GetPeerCount()` returns ALL peers; iterate and check `PT_GetPeerState(p) == PT_PEER_CONNECTED`

### Dependencies

- **PeerTalk SDK**: `~/peertalk` — included via CMake `add_subdirectory()`
- **clog**: `~/Desktop/clog` — minimal C89 logging library
- **Retro68**: `~/Retro68-build/toolchain` — Classic Mac cross-compiler (fork: github.com/matthewdeaves/Retro68)

## Build Commands

### POSIX
```bash
cmake -B build -DCLOG_DIR=~/Desktop/clog && cmake --build build
# Output: build/csend-posix
```

### 68k MacTCP
```bash
export RETRO68=~/Retro68-build/toolchain
cmake -B build-68k \
  -DCMAKE_TOOLCHAIN_FILE=$RETRO68/m68k-apple-macos/cmake/retro68.toolchain.cmake \
  -DPT_PLATFORM=MACTCP -DCLOG_DIR=~/Desktop/clog \
  -DCLOG_LIB_DIR=~/Desktop/clog/build-68k
cmake --build build-68k
# Output: build-68k/csend-mac.{APPL,bin,dsk}
```

### PPC OpenTransport
```bash
cmake -B build-ppc-ot \
  -DCMAKE_TOOLCHAIN_FILE=$RETRO68/powerpc-apple-macos/cmake/retroppc.toolchain.cmake \
  -DPT_PLATFORM=OT -DCLOG_DIR=~/Desktop/clog \
  -DCLOG_LIB_DIR=~/Desktop/clog/build-ppc
cmake --build build-ppc-ot
```

### 68k OpenTransport
```bash
cmake -B build-68k-ot \
  -DCMAKE_TOOLCHAIN_FILE=$RETRO68/m68k-apple-macos/cmake/retro68.toolchain.cmake \
  -DPT_PLATFORM=OT -DCLOG_DIR=~/Desktop/clog \
  -DCLOG_LIB_DIR=~/Desktop/clog/build-68k
cmake --build build-68k-ot
```

### PPC MacTCP
```bash
cmake -B build-ppc-mactcp \
  -DCMAKE_TOOLCHAIN_FILE=$RETRO68/powerpc-apple-macos/cmake/retroppc.toolchain.cmake \
  -DPT_PLATFORM=MACTCP -DCLOG_DIR=~/Desktop/clog \
  -DCLOG_LIB_DIR=~/Desktop/clog/build-ppc
cmake --build build-ppc-mactcp
```

## Running

### POSIX
```bash
./build/csend-posix <username>
# Commands: /list, /send <peer_num> <msg>, /broadcast <msg>, /debug, /quit, /help, /test
```

### Machine Mode
```bash
./build/csend-posix <username> --machine-mode
```

### Classic Mac
Deploy `.bin` or `.APPL` to real Classic Mac hardware via FTP or disk image.

## Key Files

| File | Purpose |
|------|---------|
| `posix/main.c` | POSIX entry point, 2-thread model |
| `posix/peertalk_bridge.c` | PT callbacks, command queue (SPSC ring buffer) |
| `posix/commands.c` | Command handlers (/send, /broadcast, /list, etc.) |
| `posix/ui_terminal_interactive.c` | Interactive terminal UI |
| `posix/ui_terminal_machine.c` | JSON machine mode UI |
| `classic_mac/main.c` | Classic Mac entry point, event loop |
| `classic_mac/peertalk_bridge.c` | PT callbacks -> AppendToMessagesTE |
| `classic_mac/dialog.c` | Dialog management, send button handler |
| `shared/test.c` | Async test framework (index-based peer addressing) |
| `shared/classic_mac/ui/dialog_peerlist.c` | Peer list using PT_GetPeer* |
| `CMakeLists.txt` | Build system (POSIX + Classic Mac targets) |

## Important Notes

- **Port conflict**: PeerTalk uses fixed ports 7353-7355. Two instances can't run on the same host without Docker
- **Resource forks**: `.finf/` and `.rsrc/` folders maintain Mac metadata — preserve these
- **Cross-compiled clog**: Classic Mac builds need clog built with matching Retro68 toolchain. Use `-DCLOG_LIB_DIR=` to point at the right one
- **`__CLASSIC_MAC__` define**: Set by CMakeLists.txt for Classic Mac builds, used in shared/test.c for TickCount() vs time() abstraction
