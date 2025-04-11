# P2P Terminal Chat Application (csend)

A simple peer-to-peer chat application built in C. This project aims to support multiple platforms, starting with POSIX systems (Linux, macOS) and Classic Macintosh (System 7.x via Retro68).

Currently, the POSIX version is functional and utilizes UDP for peer discovery on the local network and TCP for direct messaging between peers. It is multi-threaded and includes a basic command-line interface. Docker support is provided for easy setup and testing of the POSIX version.

The Classic Mac version is under development. It initializes networking (MacTCP driver, DNR, gets local IP), sets up a UDP broadcast endpoint, manages a peer list using shared code, logs to a file and the screen, displays a GUI, and handles basic events like button clicks and window closure. Message sending logic is partially implemented but network transmission is not yet complete.

Detailed information on the versions of the project can be found [here](TAGS.md). I'm building this out in a way to help others learn and follow along with tags to document the major stages of development from simple client/server apps through to a combined and then shared set C source for different platforms.

## Project Structure

*   `posix/`: Source code specific to the POSIX (Linux, macOS) version.
*   `classic_mac/`: Source code specific to the Classic Macintosh (System 7.x) version. Includes `DNR.c` library.
*   `shared/`: Platform-independent code shared between versions (e.g., communication protocol, peer list logic).
*   `Makefile`: Builds the POSIX version.
*   `Makefile.classicmac`: Builds the Classic Mac version using Retro68.
*   `Dockerfile`, `docker-compose.yml`, `docker.sh`: Support files for running the POSIX version in Docker containers.
*   `setup_retro68.sh`: Script to download, build and setup your PATH for retro68 ready to compile the classic Mac binary.
*   `tools/`: Contains helper tools like PMD for static analysis.
*   `misc/`: Miscellaneous files like original Mac OS interfaces.
*   `resedit.md`: Explanation of using ResEdit and resource conversion.
*   `TAGS.md`: Documents major development stages.

## Features (POSIX Version)

*   **Peer Discovery:** Automatically discovers other peers running on the same local network using UDP broadcasts (`posix/discovery.c`, `posix/discovery.h`).
*   **Direct Messaging:** Sends and receives text messages directly between peers using TCP connections (`posix/messaging.c`, `posix/messaging.h`).
*   **Peer Management:** Maintains a list of active peers using shared logic, updating their status and handling timeouts (`posix/peer.c`, `shared/peer_shared.c`, `shared/common_defs.h`).
*   **Terminal UI:** Provides a simple command-line interface for user interaction (`posix/ui_terminal.c`, `posix/ui_terminal.h`).
*   **Command Handling:** Supports commands like `/list`, `/send`, `/broadcast`, `/quit`, and `/help`.
*   **Multi-threading:** Uses Pthreads to handle user input, network listening (TCP), and peer discovery (UDP) concurrently.
*   **Graceful Shutdown:** Handles `SIGINT` (Ctrl+C) and `SIGTERM` signals for clean termination, notifying other peers (`posix/signal_handler.c`, `posix/signal_handler.h`).
*   **Network Utilities:** Includes helpers for getting the local IP address and setting socket timeouts (`posix/network.c`, `posix/network.h`).
*   **Simple Protocol:** Uses a basic text-based protocol (`TYPE|SENDER@IP|CONTENT`) for communication (`shared/protocol.c`, `shared/protocol.h`).
*   **Logging:** Basic timestamped logging to stdout (`posix/logging.c`, `posix/logging.h`).
*   **Docker Support:** Includes `Dockerfile`, `docker-compose.yml`, and a helper script (`docker.sh`) to easily build and run multiple peer instances of the POSIX version in containers.

## Features (Classic Mac Version)

*   **Networking Init:** Initializes MacTCP driver, gets local IP address, initializes DNR (`classic_mac/network.c`).
*   **UDP Discovery:** Initializes UDP endpoint and sends periodic discovery broadcasts (`classic_mac/discovery.c`).
*   **Peer Management:** Initializes and maintains the peer list using shared code (`classic_mac/peer_mac.c`, `shared/peer_shared.c`). Prunes timed-out peers.
*   **Basic GUI:** Uses ResEdit-defined resources (`classic_mac/csend.r`) managed by the Dialog Manager (`classic_mac/dialog.c`). Includes:
    *   Message display area (TextEdit).
    *   Message input area (TextEdit).
    *   Send button.
    *   Broadcast checkbox.
    *   Placeholder for peer list (List Manager not yet implemented).
*   **Event Loop:** Handles basic Mac OS events (mouse clicks, window updates, activation) (`classic_mac/main.c`).
*   **Partial Send Logic:** Handles 'Send' button click, retrieves input text, checks broadcast state, formats message via shared protocol, finds target peer IP (if not broadcast), but actual network send is not yet implemented (`classic_mac/dialog.c`).
*   **Logging:** Logs messages to a file (`csend_log.txt`) and appends them to the message display area in the GUI (`classic_mac/logging.c`).
*   **Quit Support:** Allows quitting via the window's close box.

## Prerequisites

### For Building POSIX Version Locally:

*   A C compiler (like `gcc`)
*   `make` build tool
*   A POSIX-compliant operating system (Linux, macOS recommended) supporting Pthreads and standard socket libraries.

### For Running POSIX Version with Docker:

*   Docker Engine
*   Docker Compose
*   A compatible terminal emulator (`gnome-terminal`, `xterm`, `konsole`, macOS `Terminal`) for the `docker.sh` script's terminal opening feature.

### For Building Classic Mac Version:

*   Retro68 cross-compiler toolchain [https://github.com/autc04/Retro68](https://github.com/autc04/Retro68). You can run `./setup_retro68.sh` to download, build and setup retro68.
*   Classic Mac OS environment (e.g., via QEMU) for testing (you could consider using my other project [QemuMac](https://github.com/matthewdeaves/QemuMac)).
*   A tool like [binUnpk](https://www.macintoshrepository.org/74045-binunpk) on the Classic Mac environment to unpack the compiled binary.

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
    make -f Makefile.classicmac
    ```
    This will create `build/classic_mac/csend-mac.bin` (packed binary), `build/classic_mac/csend-mac.APPL` (resource fork only, for some emulators), and `build/classic_mac/csend-mac.dsk` (disk image). The `.bin` file is generally the most portable for transferring.

#### A Note on ResEdit and .rsrc files
I use ResEdit on a Mac VM to create the GUI for csend, saved as a csend.rsrc file. You can't work with .rsrc files on modern Ubuntu/macOS and have to convert the .rsrc file into a text format that retro68 can work with. I've written a detailed explanation of this [here](resedit.md)

## Running

### POSIX Version Locally

Run the compiled executable from your terminal. You can optionally provide a username as a command-line argument. If no username is provided, it defaults to "anonymous".

```bash
./build/posix/csend_posix alice
```

### POSIX Version With Docker (Recommended for Testing)
The provided `docker.sh` script simplifies running multiple POSIX peers in isolated containers using Docker Compose.

Start the containers:
```bash
./docker.sh start
```
This command will:
*   Build the Docker image using the Dockerfile.
*   Start three containers (peer1, peer2, peer3) based on the `docker-compose.yml` configuration.
*   Assign static IPs within a dedicated Docker network (192.168.150.2, 192.168.150.3, 192.168.150.4).
*   Attempt to open a new terminal window attached to each running container. (Requires `gnome-terminal`, `xterm`, `konsole`, or macOS `Terminal`).

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
*   `/broadcast <message>`: Send a `<message>` to all currently active peers.
*   `/quit`: Send a quit notification to all peers, gracefully shut down the application, and exit.
*   `/help`: Display the list of available commands.

### Classic Mac Version

1.  Use a tool or method (like my [QemuMac](https://github.com/matthewdeaves/QemuMac) project's shared drive feature) to transfer the compiled `build/classic_mac/csend-mac.bin` file to your Classic Mac OS environment (VM or real hardware).
2.  On the Classic Mac, use a utility like `binUnpk` to unpack the `csend-mac.bin` file. This will create the actual application file (likely named `csend-mac`).
3.  Double-click the unpacked application file to run it.