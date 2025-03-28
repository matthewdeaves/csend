# P2P Terminal Chat Application

A simple peer-to-peer chat application built in C that runs in the terminal. It utilizes UDP for peer discovery on the local network and TCP for direct messaging between peers. The application is multi-threaded and includes a basic command-line interface. It also comes with Docker support for easy setup and testing.

Detailed information on the versions can be found [here](TAGS.md)

## Features

*   **Peer Discovery:** Automatically discovers other peers running on the same local network using UDP broadcasts (`discovery.c`, `discovery.h`).
*   **Direct Messaging:** Sends and receives text messages directly between peers using TCP connections (`messaging.c`, `messaging.h`).
*   **Peer Management:** Maintains a list of active peers, updating their status and handling timeouts (`peer.c`, `peer.h`).
*   **Terminal UI:** Provides a simple command-line interface for user interaction (`ui_terminal.c`, `ui_terminal.h`).
*   **Command Handling:** Supports commands like `/list`, `/send`, `/broadcast`, `/quit`, and `/help`.
*   **Multi-threading:** Uses Pthreads to handle user input, network listening (TCP), and peer discovery (UDP) concurrently.
*   **Graceful Shutdown:** Handles `SIGINT` (Ctrl+C) and `SIGTERM` signals for clean termination, notifying other peers (`signal_handler.c`, `signal_handler.h`).
*   **Network Utilities:** Includes helpers for getting the local IP address and setting socket timeouts (`network.c`, `network.h`).
*   **Simple Protocol:** Uses a basic text-based protocol (`TYPE|SENDER@IP|CONTENT`) for communication (`protocol.c`, `protocol.h`).
*   **Logging:** Basic timestamped logging to stdout (`utils.c`, `utils.h`).
*   **Docker Support:** Includes `Dockerfile`, `docker-compose.yml`, and a helper script (`docker.sh`) to easily build and run multiple peer instances in containers.

## Prerequisites

### For Building Locally:

*   A C compiler (like `gcc`)
*   `make` build tool
*   A POSIX-compliant operating system (Linux, macOS recommended) supporting Pthreads and standard socket libraries.

### For Running with Docker:

*   Docker Engine
*   Docker Compose

## Building

1.  Clone the repository:
```bash
git clone git@github.com:matthewdeaves/csend.git
cd csend
```
2.  Compile the application using the Makefile:
```bash
make
```
This will create an executable file named `p2p_chat`.

## Running

### Locally

Run the compiled executable from your terminal. You can optionally provide a username as a command-line argument. If no username is provided, it defaults to "anonymous".

```bash
./p2p_chat alice
```

### With Docker (Recommended for Testing)
The provided docker.sh script simplifies running multiple peers in isolated containers using Docker Compose.

Start the containers:
```bash
./docker.sh start
```

This command will:

1. Build the Docker image using the Dockerfile.
2. Start three containers (peer1, peer2, peer3) based on the docker-compose.yml configuration.
3. Assign static IPs within a dedicated Docker network (172.20.0.2, 172.20.0.3, 172.20.0.4).
4. Attempt to open a new terminal window attached to each running container. (Requires gnome-terminal, xterm, konsole, or macOS terminal).

Interact with each peer in its dedicated terminal window. They should discover each other automatically.
To stop the containers:
```bash
./docker.sh stop
```
To check the status of the containers:
bash

    ./docker.sh status

Note: To detach from a container's terminal without stopping it, use the key sequence Ctrl+P followed by Ctrl+Q. Do a Ctrl+C if you want to test SIGINT handling and graceful shutdown.

Usage Commands
Once the application is running, you can use the following commands in the terminal:

* /list: Show the list of currently known active peers, their assigned number, username, IP address, and when they were last seen.
* /send <peer_number> <message>: Send a private <message> to the peer identified by <peer_number> from the /list command.
* /broadcast <message>: Send a <message> to all currently active peers.
* /quit: Send a quit notification to all peers, gracefully shut down the application, and exit.
* /help: Display the list of available commands.