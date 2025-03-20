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

# Copy source files
COPY discovery.h peer.h ui_terminal.h ./
COPY discovery.c network.c peer.c protocol.c ui_terminal.c ./

# Compile the application
RUN gcc -o p2p_chat discovery.c network.c peer.c protocol.c ui_terminal.c -lpthread

# Default command - will be overridden by docker-compose
CMD ["/app/p2p_chat", "user"]