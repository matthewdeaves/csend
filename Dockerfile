FROM ubuntu:22.04

# Install build tools and dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    net-tools \
    iputils-ping \
    iproute2 \
    && rm -rf /var/lib/apt/lists/*

# Create app directory
WORKDIR /app

# Copy all header files
COPY discovery.h peer.h ui_terminal.h utils.h signal_handler.h network.h protocol.h ./

# Copy all source files
COPY discovery.c network.c peer.c protocol.c ui_terminal.c utils.c signal_handler.c ./

# Compile the application
RUN gcc -Wall -Wextra -o p2p_chat discovery.c network.c peer.c protocol.c ui_terminal.c utils.c signal_handler.c -lpthread

# Default command - will be overridden by docker-compose
CMD ["/app/p2p_chat", "user"]