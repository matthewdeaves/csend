# P2P Terminal Chat Application (csend)

A simple peer-to-peer chat application built in C. This project aims to support multiple platforms, starting with POSIX systems (Linux, macOS) and Classic Macintosh (System 7.x via Retro68).

Currently, the POSIX version is functional and utilizes UDP for peer discovery on the local network and TCP for direct messaging between peers. It is multi-threaded and includes a basic command-line interface. Docker support is provided for easy setup and testing of the POSIX version.

The Classic Mac version is under development and so far just displayed a GUI and allows you to quit the program by clicking the close window button.

Detailed information on the versions of the project can be found [here](TAGS.md). I'm building this out in a way to help others learn and follow along with tags to document the major stages of development from simple slient server apps through to a combined and then shared set C source for different platforms.

## Project Structure

*   `posix/`: Source code specific to the POSIX (Linux, macOS) version.
*   `classic_mac/`: Source code specific to the Classic Macintosh (System 7.x) version (under development).
*   `shared/`: Platform-independent code shared between versions (e.g., communication protocol).
*   `Makefile`: Builds the POSIX version.
*   `Dockerfile`, `docker-compose.yml`, `docker.sh`: Support files for running the POSIX version in Docker containers.
* `setup_retro68.sh` : Script to download, build and setup your PATH for retro68 ready to compile the classic Mac binary

## Features (POSIX Version)

*   **Peer Discovery:** Automatically discovers other peers running on the same local network using UDP broadcasts (`posix/discovery.c`, `posix/discovery.h`).
*   **Direct Messaging:** Sends and receives text messages directly between peers using TCP connections (`posix/messaging.c`, `posix/messaging.h`).
*   **Peer Management:** Maintains a list of active peers, updating their status and handling timeouts (`posix/peer.c`, `shared/peer.h`).
*   **Terminal UI:** Provides a simple command-line interface for user interaction (`posix/ui_terminal.c`, `posix/ui_terminal.h`).
*   **Command Handling:** Supports commands like `/list`, `/send`, `/broadcast`, `/quit`, and `/help`.
*   **Multi-threading:** Uses Pthreads to handle user input, network listening (TCP), and peer discovery (UDP) concurrently.
*   **Graceful Shutdown:** Handles `SIGINT` (Ctrl+C) and `SIGTERM` signals for clean termination, notifying other peers (`posix/signal_handler.c`, `posix/signal_handler.h`).
*   **Network Utilities:** Includes helpers for getting the local IP address and setting socket timeouts (`posix/network.c`, `posix/network.h`).
*   **Simple Protocol:** Uses a basic text-based protocol (`TYPE|SENDER@IP|CONTENT`) for communication (`shared/protocol.c`, `shared/protocol.h`).
*   **Logging:** Basic timestamped logging to stdout (`posix/logging.c`, `posix/logging.h`).
*   **Docker Support:** Includes `Dockerfile`, `docker-compose.yml`, and a helper script (`docker.sh`) to easily build and run multiple peer instances of the POSIX version in containers.

## Features (Classic Mac Version)

The Mac Version is very simple as I learn how to get a simple event driven program structure working. So at the moment the app is very basic.

*   **Basic GUI:** I built a GUI using ResEdit on a virtual Quadra 800 using Qemu and my [tools to make that easy](https://github.com/matthewdeaves/QemuMac). Because I am writing C code for classic Mac in VSCode I have to then use DeRez on the classic Mac to convert the .resource file into a text representation that retro68 can work with. I will explain more of this in future. The GUI has:
    - two User Controls that are invisible (but will be used to show the peer list and the message history)
    - A text box for you to type a message
    - A Send button to send a message
    - Support for quitting the app by clicking the close window button.

I've done the ground work to begin the process of sharing C code between the POSIX and classic Mac version. I'll start with the message protocol code.

## Prerequisites

### For Building POSIX Version Locally:

*   A C compiler (like `gcc`)
*   `make` build tool
*   A POSIX-compliant operating system (Linux, macOS recommended) supporting Pthreads and standard socket libraries.

### For Running POSIX Version with Docker:

*   Docker Engine
*   Docker Compose

### For Building Classic Mac Version:

*   Retro68 cross-compiler toolchain [https://github.com/autc04/Retro68](https://github.com/autc04/Retro68). You can run `./setup_retro68.sh` to download, build and setup retro68
*   Classic Mac OS environment (e.g., via QEMU) for testing (you could consider using my other project [QemuMac](https://github.com/matthewdeaves/QemuMac))

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

You need to have 

1.  Clone the repository:
    ```bash
    git clone git@github.com:matthewdeaves/csend.git
    cd csend
    ```
2.  Compile the application using the Makefile:
    ```bash
    make -f Makefile.classicmac
    ```
    This will create `build/classic_mac/csend-mac.bin` which can be moved to a classic Mac and then expanded using [binUnpk](https://www.macintoshrepository.org/74045-binunpk) resulting in a runnable application.

#### A Note on ResEdit and .rsrc files
I use ResEdit on a Mac VM to create the GUI for csend, saved as a csend.rsrc file. You can't work with .rsrc files on modern Ubuntu/macOS and have to convert the .rsrc file into a text format that retro68 can work with. I've written a detailed explanation of this [here](resedit.md)

## Running

### POSIX Version Locally

Run the compiled executable from your terminal. You can optionally provide a username as a command-line argument. If no username is provided, it defaults to "anonymous".

```bash
./csend_posix alice
```

### POSIX Version With Docker (Recommended for Testing)
The provided docker.sh script simplifies running multiple POSIX peers in isolated containers using Docker Compose.Start the containers:

```bash
./docker.sh start
```

This command will:

* Build the Docker image using the Dockerfile.
* Start three containers (peer1, peer2, peer3) based on the docker-compose.yml configuration.
* Assign static IPs within a dedicated Docker network (172.20.0.2, 172.20.0.3, 172.20.0.4).
* Attempt to open a new terminal window attached to each running container. (Requires gnome-terminal, xterm, konsole, or macOS terminal).

Interact with each peer in its dedicated terminal window. They should discover each other automatically.To stop the containers:

```bash
./docker.sh stop
```

To check the status of the containers:
```bash
./docker.sh status
```

Note: To detach from a container's terminal without stopping it, use the key sequence Ctrl+P followed by Ctrl+Q. Do a Ctrl+C if you want to test SIGINT handling and graceful shutdown.
Usage Commands (POSIX Terminal UI)
Once the POSIX application is running (locally or in Docker), you can use the following commands in the terminal:

* /list: Show the list of currently known active peers, their assigned number, username, IP address, and when they were last seen.
* /send <peer_number> <message>: Send a private <message> to the peer identified by <peer_number> from the /list command.
* /broadcast <message>: Send a <message> to all currently active peers.
* /quit: Send a quit notification to all peers, gracefully shut down the application, and exit.
* /help: Display the list of available commands.

### Classic Mac Version

I suggest you use my [QemuMac](https://github.com/matthewdeaves/QemuMac) project to run a VM of Mac OS 7.x using Qemu. It has two scripts that work with the same config files to either run a virtual classic Mac or to mount a shared drive image on the host Ubuntu/Linux machine allowing you to copy files (such as the compiled `build/classic_mac/csend-mac.bin`) file to be uncompressed on the VM and then run.