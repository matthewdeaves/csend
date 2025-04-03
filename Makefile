# Makefile for csend POSIX build

# Compiler and flags
CC = gcc
CFLAGS = -g -Wall -Wextra # -g for debugging, -Wall/-Wextra for warnings
LDFLAGS = -lpthread       # Link with the pthreads library

# Directories
POSIX_DIR = posix
SHARED_DIR = shared
OBJ_DIR = obj

# Include paths
INCLUDE_PATHS = -I$(POSIX_DIR) -I$(SHARED_DIR)

# Source files
POSIX_C_FILES = $(wildcard $(POSIX_DIR)/*.c)
SHARED_C_FILES = $(wildcard $(SHARED_DIR)/*.c)

# Object files (place in OBJ_DIR, prefix to avoid name clashes)
POSIX_OBJS = $(patsubst $(POSIX_DIR)/%.c, $(OBJ_DIR)/posix_%.o, $(POSIX_C_FILES))
SHARED_OBJS = $(patsubst $(SHARED_DIR)/%.c, $(OBJ_DIR)/shared_%.o, $(SHARED_C_FILES))
OBJS = $(POSIX_OBJS) $(SHARED_OBJS)

# Target executable name
TARGET = csend_posix

# Default target
all: $(TARGET)

# Rule to link the executable
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

# Rule to compile posix source files into object files
# Depends on the .c file and any .h file in posix or shared directories
# Creates the object file in $(OBJ_DIR)
$(OBJ_DIR)/posix_%.o: $(POSIX_DIR)/%.c $(wildcard $(POSIX_DIR)/*.h) $(wildcard $(SHARED_DIR)/*.h) | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDE_PATHS) -c $< -o $@

# Rule to compile shared source files into object files
# Depends on the .c file and any .h file in the shared directory
# Creates the object file in $(OBJ_DIR)
$(OBJ_DIR)/shared_%.o: $(SHARED_DIR)/%.c $(wildcard $(SHARED_DIR)/*.h) | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDE_PATHS) -c $< -o $@

# Rule to create the object directory if it doesn't exist
# The pipe symbol '|' makes this an order-only prerequisite
$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

# Rule to clean up build files
clean:
	@echo "Cleaning up..."
	@rm -rf $(OBJ_DIR) $(TARGET)

# Declare 'all' and 'clean' as phony targets (not actual files)
.PHONY: all clean
