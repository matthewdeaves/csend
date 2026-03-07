<!--
  Sync Impact Report
  ===================
  Version change: N/A -> 1.0.0 (initial ratification)
  Added principles: I-X (all new)
  Added sections: Platform Constraints, What Ships / What Does Not Ship,
                  Definition of Done, Governance
  Removed sections: none (initial)
  Templates requiring updates:
    - .specify/templates/plan-template.md: no changes needed (Constitution
      Check section is generic and will be filled per-feature)
    - .specify/templates/spec-template.md: no changes needed
    - .specify/templates/tasks-template.md: no changes needed
  Follow-up TODOs: none
-->

# CSend Constitution

## Core Principles

### I. Chat Is the Only Feature

csend does one thing: peer-to-peer text chat on a LAN. No file transfer,
no games, no plugins, no extensibility hooks. Every line of code MUST serve
the chat use case. If a proposed feature does not directly enable sending or
receiving text messages between peers, it does not belong in this project.

### II. PeerTalk Is the Network Layer

All networking — discovery, connections, messaging, reliability — is handled
by the PeerTalk SDK (`~/peertalk`). csend MUST NOT touch sockets, TCP, UDP,
or implement any discovery protocol directly. If a networking behavior needs
to change, that change belongs in PeerTalk, not here. csend calls
`PT_Init`, `PT_Poll`, `PT_Send`, `PT_Connect`, and registers callbacks.
Nothing more.

### III. Two UIs, One App

csend has two UI implementations:

- **csend-classic**: Classic Mac OS Toolbox UI (Windows, TextEdit, menus,
  dialogs) for 68k and PPC targets
- **csend-posix**: Terminal UI for Linux and macOS

Platform-specific code lives in `classic_mac/` or `posix/`. Shared
definitions live in `shared/`. Platform code MUST NOT leak into shared code.
Shared code MUST NOT import platform headers. The `ui_operations_t` strategy
pattern and `shared/common_defs.h` define the boundary.

### IV. Classic Mac Is a First-Class Target

The Classic Mac build is not a novelty or proof of concept. It MUST work
well on real hardware, not just compile. Features that cannot be implemented
on Classic Mac MUST be clearly justified and isolated behind platform
guards. The Classic Mac experience drives design decisions equally with
POSIX.

### V. C89 for Portability

All code in `shared/` and `classic_mac/` MUST compile as C89 — the Retro68
cross-compiler enforces this. POSIX-only code in `posix/` may use C11.
Never use C99/C11 features (variable-length arrays, `//` comments,
mixed declarations and code, `_Bool`, `<stdint.h>`) in shared or Classic
Mac code.

### VI. Keep It Simple

csend is a chat application, not a framework. Minimize abstraction layers.
No over-engineering, no plugin architectures, no configuration systems
beyond what is needed to start the app. Three similar lines of code are
better than a premature abstraction. Add complexity only when the current
code demonstrably cannot handle a real requirement.

### VII. Pre-Allocate Where Possible

Classic Mac targets have 4-8 MB of RAM on 68000-68040 or PPC 603/604 CPUs.
The message send/receive path MUST NOT call `malloc`. Pre-allocate buffers
at startup. Use stack allocation or static buffers for transient data.
`BUFFER_SIZE` (1024) in `shared/common_defs.h` defines the standard message
buffer.

### VIII. Poll-Based, No Threads

The main event loop drives everything. On Classic Mac: `WaitNextEvent` +
`PT_Poll` in `HandleIdleTasks`. On POSIX: the main thread runs `PT_Poll` +
command queue processing; the stdin input thread is the sole exception, and
it communicates via an SPSC ring buffer — it MUST NOT call any PeerTalk
functions directly. No other threads. No async runtimes. No callback
dispatch queues.

### IX. Logging Is Separate

Logging uses the external `clog` library (`~/Desktop/clog`). Log output
MUST NEVER appear in the user-facing UI. Use `CLOG_INFO`, `CLOG_ERR`,
`CLOG_WARN`, `CLOG_DEBUG` macros. Log levels are controlled via
`clog_set_level()`. Logging is a development/debugging tool, not a user
feature.

### X. Test on Real Hardware

The definition of "working" is: runs correctly on actual Classic Macs and
a Linux peer communicating over a real LAN. The test matrix:

| Machine | CPU | Network Stack | Build |
|---------|-----|---------------|-------|
| Mac SE | 68k | MacTCP | `build-68k/csend-mac.bin` |
| Performa 6200 | PPC | MacTCP | `build-ppc-mactcp/csend-mac.bin` |
| Performa 6400 | PPC | Open Transport | `build-ppc-ot/csend-mac.bin` |
| Linux host | x86_64 | POSIX | `build/csend-posix` |

If it does not work on real iron, it does not work.

## Platform Constraints

### Build Targets

| Target | Toolchain | CMake Config | Output |
|--------|-----------|--------------|--------|
| POSIX | Host GCC/Clang, C11 | `-B build` | `build/csend-posix` |
| 68k MacTCP | Retro68 m68k, C89 | `-B build-68k -DPT_PLATFORM=MACTCP` | `build-68k/csend-mac.bin` |
| 68k OT | Retro68 m68k, C89 | `-B build-68k-ot -DPT_PLATFORM=OT` | `build-68k-ot/csend-mac.bin` |
| PPC MacTCP | Retro68 PPC, C89 | `-B build-ppc-mactcp -DPT_PLATFORM=MACTCP` | `build-ppc-mactcp/csend-mac.bin` |
| PPC OT | Retro68 PPC, C89 | `-B build-ppc-ot -DPT_PLATFORM=OT` | `build-ppc-ot/csend-mac.bin` |

### External Dependencies

- **PeerTalk SDK**: `~/peertalk` — included via `add_subdirectory()`
- **clog**: `~/Desktop/clog` — cross-compiled per target platform
- **Retro68**: `~/Retro68-build/toolchain` — Classic Mac cross-compiler

### Resource Forks

`.finf/` and `.rsrc/` directories in `MPW_resources/` contain Mac resource
fork metadata. These MUST be preserved in version control. Never
auto-format, clean, or delete these directories.

## What Ships / What Does Not Ship

### What Ships

- `csend-posix`: POSIX terminal chat client (interactive + machine mode)
- `csend-mac`: Classic Mac chat client (68k MacTCP, 68k OT, PPC MacTCP,
  PPC OT — four binaries from one source tree)
- User commands: `/list`, `/send`, `/broadcast`, `/debug`, `/quit`,
  `/help`, `/test`
- Automatic peer discovery and connection via PeerTalk
- Chat message display with sender identification

### What Does Not Ship

- File transfer
- Encryption or authentication
- Server components — csend is fully peer-to-peer
- Installer or packaging — binaries are deployed manually via FTP or
  disk image
- User configuration files — behavior is compile-time or command-line only

## Definition of Done

A feature or fix is done when:

1. All 5 CMake build targets compile without errors or new warnings
2. POSIX build runs and passes basic chat flow (connect, send, receive)
3. At least one Classic Mac build deploys to real hardware and completes
   the same chat flow with a POSIX peer
4. No `malloc` added to the message send/receive path
5. Shared and Classic Mac code compiles as C89 (no Retro68 warnings for
   C99/C11 usage)
6. Resource fork metadata in `MPW_resources/` is intact
7. No direct socket or network calls outside of PeerTalk SDK

## Governance

This constitution is the highest-authority document for csend development
decisions. All implementation work — features, bug fixes, refactors — MUST
comply with these principles.

**Amendment process**: Update this file, increment the version, and
document the change in the Sync Impact Report comment at the top. Principle
changes that remove or redefine existing principles require a MAJOR version
bump. New principles or material expansions require MINOR. Clarifications
and wording fixes require PATCH.

**Compliance**: Every feature spec and implementation plan produced by
speckit MUST include a Constitution Check that verifies alignment with
these principles. Violations MUST be explicitly justified in the plan's
Complexity Tracking table.

**Runtime guidance**: See `CLAUDE.md` at the repository root for
build commands, file index, and development workflow details.

**Version**: 1.0.0 | **Ratified**: 2026-03-07 | **Last Amended**: 2026-03-07
