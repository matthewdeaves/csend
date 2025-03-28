# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -g
LDFLAGS = -lpthread

# Target executable
TARGET = p2p_chat

# Source files and object files
# Added messaging.c to the list
SRCS = peer.c network.c protocol.c ui_terminal.c discovery.c utils.c signal_handler.c messaging.c
OBJS = $(SRCS:.c=.o)

# Ensure ALL header files are listed here for the simple dependency rule below
# Added messaging.h to the list
HEADERS = peer.h network.h protocol.h ui_terminal.h discovery.h utils.h signal_handler.h messaging.h

# Default target
all: $(TARGET)

# Link object files to create executable
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

# Compile source files to object files
# This rule automatically handles compiling .c files into .o files
# The dependency on $(HEADERS) ensures recompilation if any header changes
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up build files
clean:
	rm -f $(OBJS) $(TARGET)

# Phony targets
.PHONY: all clean