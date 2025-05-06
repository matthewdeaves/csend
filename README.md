# P2P Terminal Chat Application (csend)

A peer-to-peer chat application built in C. This project aims to support multiple platforms, starting with POSIX systems (Linux, macOS) and Classic Macintosh (System 7.x via Retro68).

Currently, the POSIX version is functional and utilizes UDP for peer discovery on the local network and TCP for direct messaging between peers. It is multi-threaded and includes a basic command-line interface. Docker support is provided for easy setup and testing of the POSIX version.

The Classic Mac version is also functional, providing a GUI-based chat experience. It initializes MacTCP networking, discovers peers using UDP broadcasts (similar to POSIX), manages a peer list, and handles TCP communication for sending and receiving messages (TEXT and QUIT). Due to the nature of MacTCP and the single-threaded event loop, it employs a strategy of asynchronously listening for incoming TCP connections and temporarily aborting this listen to perform synchronous-style outgoing TCP operations on a single, shared TCP stream.

Detailed information on the versions of the project can be found [here](TAGS.md). I'm building this out in a way to help others learn and follow along with tags to document the major stages of development from simple client/server apps through to a combined and then shared set C source for different platforms.

You can watch a [demo video on YouTube](https://www.youtube.com/watch?v=_9iXCBZ_FjE) (Note: Video shows an older version; current functionality is more advanced!).

## Project Structure

*   `posix/`: Source code specific to the POSIX (Linux, macOS) version.
*   `classic_mac/`: Source code specific to the Classic Macintosh (System 7.x) version. Includes `DNR.c` (Domain Name Resolver library).
*   `shared/`: Platform-independent code shared between versions (e.g., communication protocol, peer list management, discovery logic, messaging logic).
*   `Makefile`: Builds the POSIX version.
*   `Makefile.retro68`: Builds the Classic Mac version using Retro68.
*   `MPW_resources/`: Contains `csend.r` (Rez input) and `csend.rsrc` (ResEdit output) for the Classic Mac GUI.
*   `Dockerfile`, `docker-compose.yml`: Support files for running the POSIX version in Docker containers. (Run `./docker.sh start` to use these easily).
*   `setup_retro68.sh` (in root, assumed): Script to download, build and setup your PATH for retro68.
*   `tools/`: Contains copy paste detection tools.
*   `misc/`: Miscellaneous files for Retro68.
*   `resedit.md`: Explanation of using ResEdit and resource conversion.
*   `TAGS.md`: Documents major development stages.
*   `agentnotes.txt`: Notes for development with AI agents.

## Shared Core Logic

The `shared/` directory contains the core logic that is platform-independent:

*   **Common Definitions (`common_defs.h`):** Provides essential constants like `BUFFER_SIZE`, `MAX_PEERS`, `PORT_TCP`, `PORT_UDP`, `DISCOVERY_INTERVAL`, `PEER_TIMEOUT`, and the definition of the `peer_t` structure used for storing peer information.
*   **Protocol (`protocol.c`/`.h`):** Defines the message format (`MAGIC_NUMBER|TYPE|SENDER@IP|CONTENT`) and provides functions for formatting and parsing messages. Handles byte order differences between platforms.
*   **Peer Management (`peer_shared.c`/`.h`):** Manages the list of peers (`peer_t`), including adding, updating, finding, and pruning timed-out peers. Handles platform-specific time functions (`TickCount()` vs `time()`).
*   **Discovery Logic (`discovery_logic.c`/`.h`):** Processes incoming UDP packets (`DISCOVERY`, `DISCOVERY_RESPONSE`). Uses platform-specific callbacks to send responses, update the peer list, and notify the UI.
*   **Messaging Logic (`messaging_logic.c`/`.h`):** Processes the content of received TCP messages (`TEXT`, `QUIT`). Uses platform-specific callbacks to update peer status, display messages, and handle quit notifications.

## Features (POSIX Version)

*   **Peer Discovery:** Automatically discovers other peers on the same local network using UDP broadcasts. Implements `discovery_platform_callbacks_t` for POSIX-specific network operations and peer list updates (`posix/discovery.c`).
*   **Direct Messaging (TCP):** Sends and receives text messages and quit notifications directly between peers using TCP connections. Implements `tcp_platform_callbacks_t` for POSIX behavior (`posix/messaging.c`).
*   **Peer Management:** Maintains a list of active peers using the shared peer logic, protected by a mutex, and updates their status based on network activity and timeouts (`posix/peer.c`).
*   **Terminal UI:** Provides a simple command-line interface for user interaction (`posix/ui_terminal.c`).
*   **Command Handling:** Supports commands like `/list`, `/send <peer_number> <message>`, `/broadcast <message>`, `/quit`, and `/help`. The `/broadcast` command sends individual TCP messages to all known active peers.
*   **Multi-threading:** Uses Pthreads to handle user input, network listening (TCP), and peer discovery (UDP) concurrently (`posix/peer.c`).
*   **Graceful Shutdown:** Handles `SIGINT` (Ctrl+C) and `SIGTERM` signals for clean termination, notifying other peers by sending QUIT messages (`posix/signal_handler.c`).
*   **Network Utilities:** Includes helpers for getting the local IP address and setting socket timeouts (`posix/network.c`).
*   **Logging:** Basic timestamped logging to stdout (`posix/logging.c`).
*   **Docker Support:** Includes `Dockerfile`, `docker-compose.yml` (and a helper script `docker.sh`) to easily build and run multiple peer instances.

## Features (Classic Mac Version)

*   **Networking Stack:**
    *   Initializes MacTCP driver and DNR (Domain Name Resolver) using `DNR.c` for IP-to-string conversion (`classic_mac/network.c`).
    *   Obtains the local IP address from MacTCP.
*   **Peer Discovery (UDP):**
    *   Initializes a UDP endpoint using MacTCP (`classic_mac/discovery.c`).
    *   Sends periodic discovery broadcasts (`MSG_DISCOVERY`) synchronously.
    *   Listens for UDP packets by polling an asynchronous `UDPRead` operation.
    *   Processes received UDP packets (discovery and responses) using the shared `discovery_logic.c`, with Mac-specific callbacks for sending responses (`SendDiscoveryResponseSync`) and updating the peer list.
    *   Manages UDP receive buffers via asynchronous `UDPBfrReturn` (polled).
*   **Direct Messaging (TCP):**
    *   Manages a **single TCP stream** for both incoming and outgoing connections (`classic_mac/tcp.c`).
    *   **Incoming Connections:**
        *   Listens for incoming TCP connections on `PORT_TCP` using an asynchronous `TCPPassiveOpen` call, which is polled in the main event loop.
        *   When a connection is established, it reads data using synchronous-style polling on `TCPRcv`.
        *   Received data is parsed using the shared `protocol.c` and then processed by the shared `messaging_logic.c` with Mac-specific callbacks to display messages and update peer status.
    *   **Outgoing Connections (`TCP_SendTextMessageSync`, `TCP_SendQuitMessagesSync`):**
        *   To send a message, if a passive listen is pending, it is **aborted**.
        *   An active TCP connection is then established to the target peer using synchronous-style polling of `TCPActiveOpen`.
        *   The formatted message (TEXT or QUIT) is sent using synchronous-style polling of `TCPSend`.
        *   The connection is then immediately closed using `TCPAbort`.
        *   The system then returns to attempting a passive listen.
    *   This strategy allows a single stream to handle both listening and sending in a cooperative multitasking environment.
*   **Peer Management:**
    *   Initializes and maintains the peer list using shared code (`classic_mac/peer_mac.c`, `shared/peer_shared.c`).
    *   Prunes timed-out peers based on `TickCount()`.
    *   Updates the peer list display in the GUI.
*   **Graphical User Interface (GUI):**
    *   Uses ResEdit-defined resources (`MPW_resources/csend.r`, `MPW_resources/csend.rsrc`) managed by the Dialog Manager (`classic_mac/dialog.c`).
    *   **Message Display Area:** A TextEdit field for displaying incoming messages, logs, and sent messages (`classic_mac/dialog_messages.c`). Includes a functional scrollbar.
    *   **Message Input Area:** A TextEdit field for typing messages (`classic_mac/dialog_input.c`).
    *   **Peer List Display:** Uses a List Manager control to display active peers (username@IP) dynamically (`classic_mac/dialog_peerlist.c`). Users can select a peer from this list for direct messaging.
    *   **Controls:** "Send" button and "Broadcast" checkbox. The "Broadcast" checkbox currently only affects local display for sent messages; network transmission for broadcasted messages is not yet implemented via this checkbox.
*   **Event Loop:** A standard Classic Mac event loop (`classic_mac/main.c`):
    *   Handles mouse clicks (button presses, list selection, scrollbar interaction, window dragging/closing).
    *   Manages window updates and activation/deactivation of UI elements.
    *   Calls `TEIdle` for TextEdit fields.
    *   Periodically polls network services (`PollUDPListener`, `PollTCP`), checks for discovery broadcast intervals, and updates the peer list display.
*   **Logging:**
    *   Logs messages to a file (`csend_log.txt`) for debugging (`classic_mac/logging.c`).
    *   Appends important log messages and received/sent chat messages to the GUI's message display area.
*   **Quit Support:**
    *   Allows quitting via the window's close box.
    *   Upon quitting, attempts to send `MSG_QUIT` notifications to all known active peers using `TCP_SendQuitMessagesSync`.

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
The provided `docker.sh` script (if available, otherwise use `docker-compose`) simplifies running multiple POSIX peers in isolated containers.

Assuming `docker.sh` script:
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
*   `/quit`: Send a quit notification to all peers, gracefully shut down the application, and exit.
*   `/help`: Display the list of available commands.

### Classic Mac Version

1.  Transfer the compiled `build/classic_mac/csend-mac.bin` file to your Classic Mac OS environment (e.g., using a shared folder with an emulator, or transferring the `.dsk` image).
2.  On the Classic Mac, use `binUnpk` to decode the `csend-mac.bin` file. This will extract the actual application file (named `csend-mac`).
3.  Double-click the unpacked application file to run it.
4.  **To send a message: NOT YET IMPLEMENTED**
    *   Type your message in the lower input field.
    *   **For a direct message to a selected peer:**
        *   Ensure the "Broadcast" checkbox is **unchecked**.
        *   Select the desired recipient from the peer list by clicking on their entry.
        *   Click the "Send" button. The message will be sent via TCP to the selected peer. Your sent message will appear in the main message area, prefixed with "You (to <username>):".
    *   **For a "broadcast" message (simulated locally):**
        *   Check the "Broadcast" checkbox.
        *   Click the "Send" button. **Currently, this does not send the message over the network to all peers.** It will only display your message locally in the message area, prefixed with "You (Broadcast):". Full network broadcast functionality for messages is not yet implemented for this option.
5.  **To quit:** Click the close box on the application window. It will attempt to send QUIT messages to peers.