# Dockerfile for csend POSIX build

FROM ubuntu:22.04

# Install build tools (includes make and gcc) and dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    net-tools \
    iputils-ping \
    iproute2 \
    && rm -rf /var/lib/apt/lists/*

# Create app directory and set as working directory
WORKDIR /app

# Copy the Makefile to the root of the WORKDIR
COPY Makefile ./

# Copy the shared source code directory
COPY shared ./shared/

# Copy the posix source code directory
COPY posix ./posix/

# Compile the application using the Makefile
# The Makefile should handle finding sources in ./posix and ./shared
# and placing object files in ./obj and the final binary in ./
RUN make

# Default command - will be overridden by docker-compose,
# but good practice to have a sensible default.
# Use the executable name defined in the Makefile (csend_posix)
CMD ["/app/csend_posix", "default_user"]