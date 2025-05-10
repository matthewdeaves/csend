# P2P Terminal Chat Application (csend)

`csend` is a cross-platform peer-to-peer chat application in C, with functional versions for POSIX systems and Classic Macintosh.

The **POSIX version** is a multi-threaded, command-line application. It uses UDP for local network peer discovery and TCP for direct messaging. Docker support is provided for testing.

The **Classic Mac version** is a GUI application for System 7.x (via Retro68). It utilizes MacTCP for UDP peer discovery and TCP messaging (TEXT and QUIT types). Its single-threaded event loop manages a shared TCP stream by primarily listening asynchronously for incoming connections, and temporarily aborting this to perform synchronous outgoing TCP operations before resuming the listen.

Both versions utilize a shared C codebase for core protocol handling, peer list management, and the underlying discovery and messaging logic.
You can watch a [demo video on YouTube](https://www.youtube.com/watch?v=_9iXCBZ_FjE) (Note: Video shows an older version; current functionality is more advanced!).

## Project Structure

*   `posix/`: Source code specific to the POSIX (Linux, macOS) version.
*   `classic_mac/`: Source code specific to the Classic Macintosh (System 7.x) version. Includes `DNR.c` (Domain Name Resolver library).
*   `shared/`: Platform-independent code shared between versions (e.g., communication protocol, peer list management, discovery logic, messaging logic).
*   `Makefile`: Builds the POSIX version.
*   `Makefile.retro68`: Builds the Classic Mac version using Retro68.
*   `MPW_resources/`: Contains `csend.r` (Rez input) and `csend.rsrc` (ResEdit output) for the Classic Mac GUI.
*   `Dockerfile`, `docker-compose.yml`: Support files for running the POSIX version in Docker containers. (Run `./docker.sh start` to use these easily).
*   `setup_retro68.sh` Script to download, build and setup your PATH for retro68.
*   `tools/`: Contains copy paste detection tools.
*   `misc/`: Miscellaneous files for Retro68.
*   `resedit.md`: Explanation of using ResEdit and resource conversion.
*   `TAGS.md`: Documents major development stages.
*   `agentnotes.txt`: Notes for development with AI agents.

## Shared Core Logic

The `shared/` directory contains the core logic that is platform-independent:

*   **Common Definitions (`common_defs.h`):** Provides essential constants like `BUFFER_SIZE`, `INET_ADDRSTRLEN`, `PORT_TCP`, `PORT_UDP`, `MAX_PEERS`, `DISCOVERY_INTERVAL`, `PEER_TIMEOUT`, and the definition of the `peer_t` structure used for storing peer information (IP, username, last_seen, active status).
*   **Protocol (`protocol.c`/`.h`):** Defines the message format (`MSG_MAGIC_NUMBER|TYPE|SENDER@IP|CONTENT`) and provides functions for formatting (`format_message`) and parsing (`parse_message`) messages. Handles byte order differences for `MSG_MAGIC_NUMBER` using `htonl`/`ntohl` (or platform-specific equivalents for Classic Mac). Defines message types `MSG_DISCOVERY`, `MSG_DISCOVERY_RESPONSE`, `MSG_TEXT`, and `MSG_QUIT`.
*   **Peer Management (`peer.c`/`.h`):** Manages the list of peers (`peer_t` array within `peer_manager_t`), including initializing the list (`peer_shared_init_list`), adding/updating peers (`peer_shared_add_or_update`), finding peers by IP (`peer_shared_find_by_ip`), finding empty slots (`peer_shared_find_empty_slot`), and pruning timed-out peers (`peer_shared_prune_timed_out`). Handles platform-specific time functions (`TickCount()` for Classic Mac, `time()` for POSIX) for `last_seen` and timeout calculations.
*   **Discovery Logic (`discovery.c`/`.h`):** Processes incoming UDP packets (`discovery_logic_process_packet`) for `MSG_DISCOVERY` and `MSG_DISCOVERY_RESPONSE`. Uses platform-specific callbacks (`discovery_platform_callbacks_t`) to send responses, add/update peers in the platform's peer list, and notify the UI/peer list display of updates.
*   **Messaging Logic (`messaging.c`/`.h`):** Processes the content of received TCP messages (`handle_received_tcp_message`) for `MSG_TEXT` and `MSG_QUIT`. Uses platform-specific callbacks (`tcp_platform_callbacks_t`) to add/update peer status, display text messages in the UI, and mark peers as inactive upon receiving a `MSG_QUIT`.
*   **Logging (`logging.c`/`.h`):** Provides a common logging interface (`log_init`, `log_shutdown`, `log_debug`, `log_app_event`). Uses platform-specific callbacks (`platform_logging_callbacks_t`) for timestamp generation and displaying debug messages to the UI/console. Also handles writing logs to a platform-specific file (e.g., `app_posix.log`, `app_classic_mac.log`).

## Features (POSIX Version)

*   **Peer Discovery:** Automatically discovers other peers on the same local network using UDP broadcasts. Implements `discovery_platform_callbacks_t` for POSIX-specific network operations (sending UDP responses via `posix_send_discovery_response`) and peer list updates (via `posix_add_or_update_peer` which calls `add_peer`) (`posix/discovery.c`).
*   **Direct Messaging (TCP):** Sends and receives text messages and quit notifications directly between peers using TCP connections. Implements `tcp_platform_callbacks_t` for POSIX behavior, including adding/updating peers (`posix_tcp_add_or_update_peer`), displaying messages to the terminal (`posix_tcp_display_text_message`), and marking peers inactive (`posix_tcp_mark_peer_inactive`) (`posix/messaging.c`).
*   **Peer Management:** Maintains a list of active peers in an `app_state_t` structure, using the shared peer logic. Access to the peer list is protected by a `pthread_mutex_t`. Updates peer status based on network activity and timeouts (`posix/peer.c`).
*   **Terminal UI:** Provides a simple command-line interface for user interaction (`posix/ui_terminal.c`).
*   **Command Handling:** Supports commands like `/list`, `/send <peer_number> <message>`, `/broadcast <message>`, `/debug` (toggles debug output visibility), `/quit`, and `/help`. The `/broadcast` command sends individual TCP messages to all known active peers.
*   **Multi-threading:** Uses Pthreads to handle user input (`user_input_thread`), network listening for TCP connections (`listener_thread`), and peer discovery/UDP listening (`discovery_thread`) concurrently (`posix/main.c`).
*   **Graceful Shutdown:** Handles `SIGINT` (Ctrl+C) and `SIGTERM` signals for clean termination (`posix/signal_handler.c`). The `/quit` command also initiates shutdown, notifying other peers by sending `MSG_QUIT` messages (`posix/ui_terminal.c`).
*   **Network Utilities:** Includes helpers for getting the local IP address (`get_local_ip`) and setting socket timeouts (`set_socket_timeout`) (`posix/network.c`).
*   **Logging:** Uses the shared logging system. Platform-specific callbacks provide POSIX-style timestamps and print debug messages to `stdout` (`posix/logging.c`). Log messages are also written to `csend_posix.log`.
*   **Docker Support:** Includes `Dockerfile`, `docker-compose.yml` (and a helper script `docker.sh`) to easily build and run multiple peer instances.

## Features (Classic Mac Version)

*   **Networking Stack:**
    *   Initializes MacTCP driver using `PBOpenSync` (`classic_mac/mactcp_network.c`).
    *   Initializes DNR (Domain Name Resolver) using `DNR.c` for IP-to-string conversion (`AddrToStr`) and string-to-IP (`ParseIPv4` helper).
    *   Obtains the local IP address from MacTCP using `ipctlGetAddr`.
*   **Peer Discovery (UDP):**
    *   Initializes a UDP endpoint using MacTCP's `UDPCreate` (`classic_mac/mactcp_discovery.c`).
    *   Sends periodic discovery broadcasts (`MSG_DISCOVERY`) synchronously using `SendDiscoveryBroadcastSync`.
    *   Listens for UDP packets by polling an asynchronous `UDPRead` operation (`PollUDPListener`).
    *   Processes received UDP packets using the shared `discovery.c` (`discovery_logic_process_packet`), with Mac-specific callbacks: `mac_send_discovery_response` (uses `SendDiscoveryResponseSync`) for sending responses, `mac_add_or_update_peer` (uses `AddOrUpdatePeer`) for updating the peer list, and `mac_notify_peer_list_updated` for triggering GUI updates.
    *   Manages UDP receive buffers via asynchronous `UDPBfrReturn` (polled).
*   **Direct Messaging (TCP):**
    *   Manages a **single TCP stream** for both incoming and outgoing connections (`classic_mac/mactcp_messaging.c`).
    *   **Incoming Connections:**
        *   Listens for incoming TCP connections on `PORT_TCP` using an asynchronous `TCPPassiveOpen` call, which is polled in the main event loop (`PollTCP`).
        *   When a connection is established, it reads data using synchronous-style polling of `TCPRcv` (via `LowTCPRcvSyncPoll`).
        *   Received data is parsed using the shared `protocol.c` and then processed by the shared `messaging.c` (`handle_received_tcp_message`) with Mac-specific callbacks: `mac_tcp_add_or_update_peer`, `mac_tcp_display_text_message`, and `mac_tcp_mark_peer_inactive`.
    *   **Outgoing Connections (`MacTCP_SendMessageSync`):**
        *   To send a message, if a passive listen (`TCPPassiveOpen`) is pending, it is **aborted** using `LowTCPAbortSyncPoll`.
        *   An active TCP connection is then established to the target peer using synchronous-style polling of `TCPActiveOpen` (via `LowTCPActiveOpenSyncPoll`).
        *   The formatted message (TEXT or QUIT) is sent using synchronous-style polling of `TCPSend` (via `LowTCPSendSyncPoll`).
        *   The connection is then immediately closed using `TCPAbort` (via `LowTCPAbortSyncPoll`).
        *   The system then returns to attempting a passive listen.
    *   This strategy allows a single stream to handle both listening and sending in a cooperative multitasking environment.
*   **Peer Management:**
    *   Initializes and maintains the peer list (`gPeerManager`) using shared code (`classic_mac/peer.c`, `shared/peer.c`).
    *   Prunes timed-out peers based on `TickCount()`.
    *   Updates the peer list display in the GUI (`UpdatePeerDisplayList`).
*   **Graphical User Interface (GUI):**
    *   Uses ResEdit-defined resources (`MPW_resources/csend.r`, `MPW_resources/csend.rsrc`) managed by the Dialog Manager (`classic_mac/dialog.c`).
    *   **Message Display Area:** A TextEdit field (`gMessagesTE`) for displaying incoming messages, logs, and sent messages (`classic_mac/dialog_messages.c`). Includes a functional scrollbar (`gMessagesScrollBar`).
    *   **Message Input Area:** A TextEdit field (`gInputTE`) for typing messages (`classic_mac/dialog_input.c`).
    *   **Peer List Display:** Uses a List Manager control (`gPeerListHandle`) to display active peers (username@IP) dynamically (`classic_mac/dialog_peerlist.c`). Users can select a peer from this list for direct messaging.
    *   **Controls:** "Send" button (`kSendButton`), "Broadcast" checkbox (`kBroadcastCheckbox`), and "Debug" checkbox (`kDebugCheckbox`). The "Broadcast" checkbox, when checked, causes messages to be sent to all active peers. The "Debug" checkbox toggles display of debug messages in the Message Display Area.
*   **Event Loop:** A standard Classic Mac event loop (`classic_mac/main.c`):
    *   Handles mouse clicks (button presses, list selection, scrollbar interaction, window dragging/closing), key down events for input, window updates, and activation/deactivation of UI elements.
    *   Calls `TEIdle` for TextEdit fields.
    *   Periodically polls network services (`PollUDPListener`, `PollTCP`) in `HandleIdleTasks`, checks for discovery broadcast intervals, and updates the peer list display.
*   **Logging:**
    *   Uses the shared logging system. Platform-specific callbacks provide Classic Mac timestamps (`classic_mac_platform_get_timestamp`) and display debug messages in the GUI's message display area (`classic_mac_platform_display_debug_log`) (`classic_mac/logging.c`).
    *   Logs messages to a file (`csend_mac.log`) for debugging.
*   **Quit Support:**
    *   Allows quitting via the window's close box.
    *   Upon quitting, attempts to send `MSG_QUIT` notifications to all known active peers using `MacTCP_SendMessageSync`.

## Prerequisites

### For Building POSIX Version Locally:

*   A C compiler (like `gcc`)
*   `make` build tool
*   A POSIX-compliant operating system (Linux, macOS recommended) supporting Pthreads and standard socket libraries.

### For Running POSIX Version with Docker (tested only on Ubuntu 24):

*   Docker Engine
*   Docker Compose
*   A compatible terminal emulator (`gnome-terminal`, `xterm`, `konsole`, maybe macOS `Terminal`) when using `docker.sh` that opens new terminals.

### For Building Classic Mac Version:

*   Retro68 cross-compiler toolchain [https://github.com/autc04/Retro68](https://github.com/autc04/Retro68). You can run `./setup_retro68.sh` to download, build and setup retro68.
*   Classic Mac OS environment (e.g., via QEMU, Basilisk II, or real hardware) for testing. Consider using a project like [QemuMac](https://github.com/matthewdeaves/QemuMac) for easy setup.
*   A tool like [binUnpk](https://www.macintoshrepository.org/74045-binunpk) or Stuffit Expander on the Classic Mac environment to unpack the compiled MacBinary (`.bin`) file.

## Building

### POSIX Version

1.  Clone the repository:
    ```bash
    git clone git@github.com:matthewdeaves/csend.git
    cd csend
    ```
2.  Compile the application using the Makefile:
    ```bash
    make
    ```
    This will create an executable file named `build/posix/csend_posix`.

### Classic Mac Version

1.  Ensure the Retro68 toolchain is installed and in your PATH (or use `./setup_retro68.sh`).
2.  Clone the repository:
    ```bash
    git clone git@github.com:matthewdeaves/csend.git
    cd csend
    ```
3.  Compile the application using the Classic Mac Makefile:
    ```bash
    make -f Makefile.retro68
    ```
    This will create:
    *   `build/classic_mac/csend-mac.APPL`: A Macintosh application bundle.
    *   `build/classic_mac/csend-mac.bin`: A MacBinary (`.bin`) encoded version of `csend-mac.APPL`, which combines data and resource forks into a single file. This is generally the most portable format for transferring the application to a Classic Mac environment or emulator.
    *   `build/classic_mac/csend-mac.dsk`: A floppy disk image containing `csend-mac`.

#### A Note on ResEdit and .rsrc files
I use ResEdit on a Mac VM to create the GUI for csend, saved as a `csend.rsrc` file (located in `MPW_resources/`). You can't easily work with `.rsrc` files directly in modern development environments. The build process uses `Rez` to compile a textual representation of resources (`MPW_resources/csend.r`) into the application's resource fork. I've written a detailed explanation of this process [here](resedit.md).

## Running

### POSIX Version Locally

Run the compiled executable from your terminal. You can optionally provide a username as a command-line argument. If no username is provided, it defaults to "anonymous".

```bash
./build/posix/csend_posix alice
```
Open another terminal and run:
```bash
./build/posix/csend_posix bob
```
They should discover each other if on the same network.

### POSIX Version With Docker (Recommended for Testing)
The provided `docker.sh` script simplifies running multiple POSIX peers in isolated containers.

Start the containers:
```bash
./docker.sh start
```
This command will:
*   Build the Docker image if not already built.
*   Start multiple containers (e.g., peer1, peer2, peer3) as defined in `docker-compose.yml`.
*   Assign static IPs within a dedicated Docker network.
*   Attempt to open a new terminal window attached to each running container.

Interact with each peer in its dedicated terminal window. They should discover each other automatically.

To stop the containers:
```bash
./docker.sh stop
```

To check the status of the containers:
```bash
./docker.sh status
```

Note: To detach from a container's terminal without stopping it, use the key sequence Ctrl+P followed by Ctrl+Q. Use Ctrl+C if you want to test SIGINT handling and graceful shutdown.

### Usage Commands (POSIX Terminal UI)
Once the POSIX application is running (locally or in Docker), you can use the following commands in the terminal:

*   `/list`: Show the list of currently known active peers, their assigned number, username, IP address, and when they were last seen.
*   `/send <peer_number> <message>`: Send a private `<message>` to the peer identified by `<peer_number>` from the `/list` command.
*   `/broadcast <message>`: Send a `<message>` to all currently active peers (iteratively sends TCP unicast messages).
*   `/debug`: Toggle detailed debug message visibility in the console.
*   `/quit`: Send a quit notification to all peers, gracefully shut down the application, and exit.
*   `/help`: Display the list of available commands.

### Classic Mac Version

1.  Transfer the compiled `build/classic_mac/csend-mac.bin` file to your Classic Mac OS environment (e.g., using a shared folder with an emulator, or transferring the `.dsk` image).
2.  On the Classic Mac, use `binUnpk` to decode the `csend-mac.bin` file. This will extract the actual application file (named `csend-mac`).
3.  Double-click the unpacked application file to run it.
4.  **To send a message:**
    *   Type your message in the lower input field.
    *   **For a direct message to a selected peer:**
        *   Ensure the "Broadcast" checkbox is **unchecked**.
        *   Select the desired recipient from the peer list by clicking on their entry.
        *   Click the "Send" button. The message will be sent via TCP to the selected peer. Your sent message will appear in the main message area, prefixed with "You (to <username>):".
    *   **For a broadcast message:**
        *   Check the "Broadcast" checkbox. (This will automatically deselect any peer in the list).
        *   Click the "Send" button. The message will be sent via TCP to all currently active peers. Your message will appear in the main message area, prefixed with "You (Broadcast):".
5.  **To toggle debug message visibility:** Click the "Debug" checkbox. Debug messages will appear in the main message area.
6.  **To quit:** Click the close box on the application window. It will attempt to send QUIT messages to all active peers before shutting down.