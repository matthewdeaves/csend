FROM ubuntu:22.04

# Install build tools (includes make and gcc) and dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    net-tools \
    iputils-ping \
    iproute2 \
    && rm -rf /var/lib/apt/lists/*

# Create app directory
WORKDIR /app

# Copy the Makefile first
COPY Makefile ./

# Copy all header files (ADDED messaging.h)
COPY discovery.h peer.h ui_terminal.h utils.h signal_handler.h network.h protocol.h messaging.h ./

# Copy all source files (ADDED messaging.c)
COPY discovery.c network.c peer.c protocol.c ui_terminal.c utils.c signal_handler.c messaging.c ./

# Compile the application using the Makefile
# This will use the CFLAGS and LDFLAGS defined in the Makefile
RUN make

# Default command - will be overridden by docker-compose
CMD ["/app/p2p_chat", "user"]