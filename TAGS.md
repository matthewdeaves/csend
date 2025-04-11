This repo is a record of my work to learn C programming with the aim of making a peer to peer enabled chat program in C that will run on Ubuntu, macOS and hopefully Mac OS System 6 with a Mac SE.

To start with I am focussing on the Ubuntu and macOS support as that will be the easiest to get back into C programming on with easy to find documentation. However, there is briliant work available to learn from by Joshua Stein such as [https://jcs.org/wikipedia](https://jcs.org/wikipedia) that are using MacTCP etc under System 6 and written in C with the Think C IDE.

I'll be tagging various points of evolution of this code base with a write up of important learnings and aspects of the code as I go.

---

## [v0.0.10](https://github.com/matthewdeaves/csend/tree/v0.0.10)

1.  **Major Refactoring: Shared Peer Management Logic:**
    *   **v0.0.9:** Peer management logic (like defining `peer_t`, adding/updating peers, checking timeouts) was implemented within the platform-specific `posix/peer.c` and absent in `classic_mac`.
    *   **v0.0.10:** Introduces `shared/peer_shared.c` and `shared/peer_shared.h`. This centralizes the core, platform-independent logic for managing the peer list:
        *   The `peer_t` structure is now defined in `shared/common_defs.h`.
        *   Functions like `peer_shared_init_list`, `peer_shared_find_by_ip`, `peer_shared_find_empty_slot`, `peer_shared_update_entry`, `peer_shared_add_or_update`, and `peer_shared_prune_timed_out` handle the fundamental operations on a `peer_t` array.
        *   Platform-specific time handling (`TickCount` vs `time`) is managed within `peer_shared.c` using `#ifdef __MACOS__`.
    *   **Benefit:** This significantly reduces code duplication, ensures consistent peer handling logic across platforms, and makes future modifications easier.

2.  **Platform-Specific Peer Wrappers:**
    *   **v0.0.10:**
        *   **POSIX (`posix/peer.c`):** This file was refactored to become a *wrapper* around the shared logic. It still manages the `app_state_t` (which contains the `peers` array and the `peers_mutex`), but functions like `add_peer` and `prune_peers` now primarily handle locking/unlocking the mutex and then call the corresponding `peer_shared_...` functions.
        *   **Classic Mac (`classic_mac/peer_mac.c`, `classic_mac/peer_mac.h`):** These *new* files provide the Classic Mac interface to the shared peer logic. They define a global `gPeerList` array and implement functions (`InitPeerList`, `AddOrUpdatePeer`, `PruneTimedOutPeers`) that directly call the `peer_shared_...` functions on this global list. A new function `GetPeerByIndex` was added specifically for the Classic Mac UI to retrieve peer data based on its visible index in the list.
    *   **Benefit:** Maintains a clear separation between the core peer logic (shared) and how each platform integrates with it (global list vs. state struct with mutex).

3.  **Logging Refactoring:**
    *   **v0.0.9:** Logging was handled by `shared/utils.c` and `shared/utils.h`, likely printing only to `stdout` (suitable for POSIX but not Classic Mac GUI).
    *   **v0.0.10:**
        *   `shared/utils.c/h` were removed.
        *   Logging functionality was moved into platform-specific implementations:
            *   `posix/logging.c/h`: Provides `log_message` which prints timestamped messages to `stdout`.
            *   `classic_mac/logging.c/h`: Provides `log_message` (renamed from `LogToDialog`) which writes to a file (`csend_log.txt`) and, if the dialog is initialized, also appends the message to the dialog's message area (`gMessagesTE`).
    *   **Benefit:** Allows each platform to log messages in the most appropriate way (console for POSIX, file/dialog for Classic Mac) while maintaining a consistent function name (`log_message`).

4.  **Classic Mac UDP Discovery Implementation:**
    *   **v0.0.9:** UDP discovery logic was likely absent or incomplete in the Classic Mac build. The `classic_mac/network.c` handled general TCP/IP setup but not UDP broadcasts specifically.
    *   **v0.0.10:**
        *   Introduced `classic_mac/discovery.c` and `classic_mac/discovery.h`.
        *   These files implement UDP broadcast functionality specifically for Classic Mac using MacTCP `PBControl` calls (`udpCreate`, `udpWrite`, `udpRelease`).
        *   `InitUDPBroadcastEndpoint` sets up the UDP stream.
        *   `SendDiscoveryBroadcast` formats and sends the discovery message.
        *   `CheckSendBroadcast` handles the periodic sending based on `DISCOVERY_INTERVAL` and `TickCount`.
        *   `CleanupUDPBroadcastEndpoint` releases the UDP stream.
        *   The UDP-related logic was removed from `classic_mac/network.c`.
    *   **Benefit:** Implements the core discovery sending mechanism for the Classic Mac platform, separating it cleanly from general TCP/IP setup.

5.  **Classic Mac Integration Updates:**
    *   **`classic_mac/main.c`:** Updated to initialize the new peer list (`InitPeerList`), initialize the UDP endpoint (`InitUDPBroadcastEndpoint`), and call `CheckSendBroadcast` and `PruneTimedOutPeers` during the idle part of the event loop.
    *   **`classic_mac/dialog.c`:** `DoSendAction` was updated to integrate with the new peer management. It now includes logic (using the new `GetPeerByIndex`) to identify the target peer IP when sending a non-broadcast message (though the actual network send call is still a TODO). It also uses the global `gMyUsername` and `gMyLocalIPStr`. The logging function name was updated from `LogToDialog` to `log_message`.

6.  **Build System Updates (Makefiles):**
    *   **`Makefile` (POSIX):** Updated to reflect the change from `shared/utils.c` to `posix/logging.c`. Object file lists and dependencies were adjusted accordingly.
    *   **`Makefile.classicmac`:** Significantly updated to:
        *   Include the new Classic Mac source files (`discovery.c`, `peer_mac.c`) and the new shared file (`peer_shared.c`) in the compilation and linking steps.
        *   Remove the old shared utility object (`shared_utils.o`).
        *   Correctly specify include paths (`-Iclassic_mac`, `-Ishared`, `-I"$(CINCLUDES)"`, `-I"$(UNIVERSAL_CINCLUDES)"`).
        *   Add dynamic detection and checking for Retro68 include paths (`RIncludes`, `CIncludes`, `Universal/CIncludes`) relative to the compiler location, making the build more robust.
        *   Use the C++ linker driver (`CXX_MAC`) which is often necessary with Retro68 even for C projects.
        *   Remove the `-lRetroConsole` linker flag as the GUI app doesn't use the console output library.

v0.0.10 represents a significant step forward in code organization and feature implementation. The core peer management logic is now shared, reducing redundancy and improving consistency. Logging is handled appropriately for each platform. UDP discovery sending is now implemented for Classic Mac. The build system is more robust, especially for the Classic Mac target. The next features to build for the classic Mac build are:

1. On quit send a `MSG_QUIT` message to peers so they will remove the peer from their peer list
2. Support for receiving `MSG_DISCOVERY_RESPONSE` message and adding a peer to the peer list

---

## [v0.0.9](https://github.com/matthewdeaves/csend/tree/v0.0.9)

Further work on code sharing between POSIX and ANSI C builds through strategic refactoring to plain C, enabling better cross-platform compatibility while maintaining platform-specific optimizations where needed.

Core Refactoring

- Refactored peer management with new _update_peer_entry helper function
    - Centralized timestamp and username update logic
    - Reduced code duplication between updating existing peers and adding new ones
    - Ensured consistent handling of NULL or empty usernames
- Moved utility code to shared directory
    - Relocated utils.c and utils.h from posix/ to shared/
    - Created shared/common_defs.h for constants used by both platforms
    - Refactored format_message() to use dependency injection for local IP

Build System Improvements

- Enhanced Classic Mac build
    - Updated Makefile.classicmac to include shared source files
    - Added RetroConsole library linking for printf output
    - Changed linker from gcc to g++ to resolve C++ symbols
- Code organization
    - Moved application constants (MAX_PEERS, DISCOVERY_INTERVAL, PEER_TIMEOUT) to common header
    - Maintained protocol constants in protocol.h for better semantic cohesion

Development Tools

- Added code duplication detection
    - New cpd_check.sh script integrates PMD's Copy/Paste Detector
    - Automatically downloads and manages PMD dependencies
    - Helps identify refactoring opportunities across the codebase
- Enhanced Docker development environment
    - Added Docker context management for both Engine and Desktop environments
    - Implemented automatic detection of connectivity issues
    - Added dependency checks and installation prompts
    - Fixed context-related issues and improved error handling
    - Adjusted IP range for Docker containers to avoid conflicts

---

## [v0.0.8](https://github.com/matthewdeaves/csend/tree/v0.0.8)

Add Classic Mac GUI Shell and Build System

- Add initial Classic Mac GUI shell (`classic_mac/main.c`) including:
    - Toolbox initialization (QuickDraw, Fonts, Windows, Menus, TE, Dialogs).
    - Main event loop (`WaitNextEvent`).
    - Loading and displaying a basic dialog window (`GetNewDialog` from resource).
    - Basic event handling (mouse down for drag/close, update events).
- Add resource definitions (`classic_mac/csend.r`) for the dialog (`DLOG`/`DITL` ID 128).
- Add Classic Mac build system (`Makefile.classicmac`) using Retro68 toolchain:
    - Compiles `classic_mac/main.c` (shared code excluded for now).
    - Uses `Rez` to combine code, resources, and `Retro68APPL.r`.
    - Dynamically locates `RIncludes` based on compiler path.
    - Outputs `.APPL`, `.bin`, `.dsk` files to `build/classic_mac/`.
- Refactor POSIX `Makefile`:
    - Organize outputs into `build/posix/` (executable) and `build/obj/posix/` (objects).
    - Update `clean` target to remove entire `build/` directory.
- Update Docker configuration (`Dockerfile`, `docker-compose.yml`) to use the new POSIX executable path (`/app/build/posix/csend_posix`).

---

## [v0.0.7](https://github.com/matthewdeaves/csend/tree/v0.0.7)

Refactor: Structure project for cross-platform build

- Created posix/, classic_mac/, and shared/ directories.
- Moved existing POSIX source code into posix/.
- Moved protocol.[ch] into shared/ as first shared module.
- Updated #include paths in posix/ code for shared/protocol.h.
- Updated root Makefile to build POSIX target from new structure.
- Updated codemerge.sh to support platform selection (posix/classic/all).
- Updated Dockerfile and docker-compose.yml for new structure.

---

## [v0.0.6](https://github.com/matthewdeaves/csend/tree/v0.0.6)

A refactor to improve code organization and modularity

- Extract utility [functions into utils.c](https://github.com/matthewdeaves/csend/blob/c86282b7e76a075df6ef3887829dd9c1b0f4bef8/utils.c) for better reusability
- Create dedicated [signal_handler.c](https://github.com/matthewdeaves/csend/blob/c86282b7e76a075df6ef3887829dd9c1b0f4bef8/signal_handler.c) for cleaner signal management
- Move messaging code into [messaging.c](https://github.com/matthewdeaves/csend/blob/c86282b7e76a075df6ef3887829dd9c1b0f4bef8/messaging.c)
- Add proper header files for all major components and streamline all includes
- Very detailed comments throughout the code to aid in learning (yes, I print and read my code with a coffee)

---

## [v0.0.5](https://github.com/matthewdeaves/csend/tree/v0.0.5)

A refactor and implementation of some development tools. Refactoring includes:

* move code related to discovery into separate [discovery.h](https://github.com/matthewdeaves/csend/blob/1feaaf242574599ae306c8125f2ba27a7b66690e/discovery.h) and [disocvery.c](https://github.com/matthewdeaves/csend/blob/1feaaf242574599ae306c8125f2ba27a7b66690e/discovery.c) files

Implementing developer tooling includes:

* Adding a [Dockerfile](https://github.com/matthewdeaves/csend/blob/1feaaf242574599ae306c8125f2ba27a7b66690e/Dockerfile), [docker-compose.yml](https://github.com/matthewdeaves/csend/blob/1feaaf242574599ae306c8125f2ba27a7b66690e/docker-compose.yml) and [p2p.sh](https://github.com/matthewdeaves/csend/blob/1feaaf242574599ae306c8125f2ba27a7b66690e/p2p.sh) enabling 3 Docker containers to be fired up each running an instance of the compiled binary for testing on local machine 

### Running Docker

Exceute `./p2p.sh start` in a terminal window (not within VSCode terminal) and the script will auto launch 3 separate terminal windows attached to each container for CLI use.

Execute `./p2p.sh stop` in any terminal window to shut them down.

---

## [v0.0.4](https://github.com/matthewdeaves/csend/tree/v0.0.4)

A very simple refactor to:

* move code related to the terminal user interface into separate files with functionally the same as [v0.0.3](https://github.com/matthewdeaves/csend/tree/v0.0.3) but with terminal UI code moved to [ui_terminal.h](https://github.com/matthewdeaves/csend/blob/390e72b0b2020471c7348b23957101f81da2588b/ui_terminal.h) and  [ui_terminal.c](https://github.com/matthewdeaves/csend/blob/390e72b0b2020471c7348b23957101f81da2588b/ui_terminal.c)

Improved developer tooling with:

* a [Makefile](https://github.com/matthewdeaves/csend/blob/e52e6e47febbaba3fb970071b84d6c0a0341260a/Makefile)
* a [codemerge.sh](https://github.com/matthewdeaves/csend/blob/dc84874cfd4fe14f15c6adbc4dafaabbf3e8d120/codemerge.sh) tool with options to combine all .c files into a single text file which can be used with LLMs to inspect the code. I find `./codemerge.sh -s dependency` gives good results with an LLM by organising the code with header files first

Compile with:

```
make
```

Run with:

```
./p2p_chat
```

View the [code](https://github.com/matthewdeaves/csend/tree/v0.0.4)

Get the [code](https://github.com/matthewdeaves/csend/releases/tag/v0.0.4)

---

## [v0.0.3](https://github.com/matthewdeaves/csend/tree/v0.0.3)

A rewrite to move to a single binary capable of sending and receiving messages with support for:
* peer to peer discovery over UDP 
* direct and broadcast messaging over TCP
* messages encapsulated with a simple messaging protocol

This is achieved by:

* implementing a [discovery_thread](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L321)
* implementing a [listener_thread](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L224)
* implementing a [user_input_thread](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/peer.c#L172)
* Moving code from previous versions into [network.c](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c) with explicit functions to initialise TCP sockets to [listen](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L53) and [send](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L165)
* Implementing a method to [initialise UDP sockets](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L142) to [broadcast discovery](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/network.c#L142) messages
* Implementing a [main method](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/peer.c#L310) to start threads
* implementing a very simple structured [message protocol](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/protocol.c)
* each thead having access to a [`struct app_state_t`](https://github.com/matthewdeaves/csend/blob/c393d6bf23b3f70750fb68d10c5c1ab0a77cf32b/peer.h#L44) to store key information for the peer such as the list of peers and a method to aid in multithreaded access to the list

The code is well commented. Both programs output to the terminal.

Compile with:

```
gcc -o p2p_chat peer.c network.c protocol.c -lpthread
```

Run with:

```
./p2p_chat
```

### Key Learnings

#### Message Protocol

It really is a very simple format which I think in future can be improved to use a more structured format with a struct and introduction of magic numbers at the start of the data and a checksum to help a peer verifiy a connection on a socket has the intended data and was sent by a valid peer. The message format is:

```
// Message format: TYPE|SENDER|CONTENT
// Example: TEXT|username@192.168.1.5|Hello, world!

```
The [parse_message()](https://github.com/matthewdeaves/csend/blob/a4cea91a61c4f70d5ce5de417bf0d7a5a40cc184/protocol.c#L38) function attempts to parse incomming messages according to the above format, 

#### Threads
Each peer maintains an array of network peers within a struct called [app_state_t]() defined in peer.h. This list of peers needs to be modified in a threadsafe manner as a peer can be added to the list via the discovery and listener threads.

To achieve this, app_state_t hold a variable of [pthread_mutex_t]() type called `peers_mutex`. This is a special type which allows the calling of functions to obtain and release a lock on that variable. For a thread to obtain the lock (which blocks execution/waits if it is not available)`pthread_mutex_lock()` is called. To release the lock `pthread_mutex_unlock()` is called. This means you have to remember to make a thread obtain the lock, do your thing and then release the lock when you are done. Both functions take an argument type `pthread_mutex_t`. For example:

```
pthread_mutex_lock(&state->peers_mutex);

// code to do modify the list of peers

pthread_mutex_unlock(&state->peers_mutex);
```

View the [code](https://github.com/matthewdeaves/csend/tree/v0.0.3)

Get the [code](https://github.com/matthewdeaves/csend/releases/tag/v0.0.3)

---

## [v0.0.2](https://github.com/matthewdeaves/csend/tree/v0.0.2)

A minor improvement to the server implementation this time in that it will run continuously listening for connections until the process is terminated (and a graceful shutdown performed). This is achieved by adding:
* [setup](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L54) and [handling](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L24) of SIGINT (Ctrl+C) and SIGTERM signals
* [non-blocking accept](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L108) of connections with a [1 second timeout via select()](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L118). This allows the servier to periodically check if it should continue to run.
* A main server [loop](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L103) which ties the above points together
* A [tidy up before shutdown](https://github.com/matthewdeaves/csend/blob/58fbc300af851ec4e6b11075ca1ead051a2cb73a/server.c#L29), closing the server sockets before program termination

The code is well commented. Both programs output to the terminal.

Compile with:

```
gcc client.c -o client
gcc server.c -o server
```

Run with:

```
./server
```

and in another terminal (run as many of these as you like or repeat whilst server is running):

```
./client
```
View the [code](https://github.com/matthewdeaves/csend/tree/v0.0.2)

Get the [code](https://github.com/matthewdeaves/csend/releases/tag/v0.0.2)

---

## [v0.0.1](https://github.com/matthewdeaves/csend/tree/v0.0.1)

This is a very simple client and server setup using sockets on the localhost. You start the [server](https://github.com/matthewdeaves/csend/blob/v0.0.1/server.c) and it will open a port, bind a socket and  listen for a connections. When a client connects it will read a message, send a response and then close connections and terminate. The call to wait for a connection is [blocking](https://github.com/matthewdeaves/csend/blob/a572ac3b9acbecea0316d38763c26d933245f092/server.c#L57) (as in the program execution pauses until a connection is made on the port).

The [client](https://github.com/matthewdeaves/csend/blob/v0.0.1/client.c) is equally simple, opening a socket, connecting to the localhost, sending a message to the server, reading a response and terminating.

The code is well commented. Both programs output to the terminal.

Compile with:

```
gcc client.c -o client
gcc server.c -o server
```

Run with:

```
./server
```

and in another terminal:

```
./client
```
View the [code](https://github.com/matthewdeaves/csend/tree/v0.0.1)

Get the [code](https://github.com/matthewdeaves/csend/releases/tag/v0.0.1)

---
