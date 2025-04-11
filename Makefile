# Makefile for csend POSIX build

# Compiler and flags
CC = gcc
CFLAGS = -g -Wall -Wextra # -g for debugging, -Wall/-Wextra for warnings
LDFLAGS = -lpthread       # Link with the pthreads library

# Directories
POSIX_DIR = posix
SHARED_DIR = shared
BUILD_BASE_DIR = build
BUILD_DIR = $(BUILD_BASE_DIR)/posix
OBJ_DIR = $(BUILD_BASE_DIR)/obj/posix

# Include paths
INCLUDE_PATHS = -I$(POSIX_DIR) -I$(SHARED_DIR)

# Source files
# POSIX_C_FILES now includes logging.c
POSIX_C_FILES = $(wildcard $(POSIX_DIR)/*.c)
# SHARED_C_FILES no longer includes utils.c (it's removed)
SHARED_C_FILES = $(wildcard $(SHARED_DIR)/*.c)

# Object files (place in OBJ_DIR)
# Use simpler names now they are in a dedicated directory
# POSIX_OBJS now includes logging.o
POSIX_OBJS = $(patsubst $(POSIX_DIR)/%.c, $(OBJ_DIR)/%.o, $(POSIX_C_FILES))
# Keep shared prefix to avoid potential name clashes if posix/ had same filenames
# SHARED_OBJS no longer includes shared_utils.o
SHARED_OBJS = $(patsubst $(SHARED_DIR)/%.c, $(OBJ_DIR)/shared_%.o, $(SHARED_C_FILES))
OBJS = $(POSIX_OBJS) $(SHARED_OBJS)

# Target executable name (with path)
TARGET = $(BUILD_DIR)/csend_posix

# Default target
all: $(TARGET)

# Rule to link the executable
# Depends on object files and ensures the build directory exists
$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

# Rule to compile posix source files into object files
# Depends on the .c file and any .h file in posix or shared directories
# Creates the object file in $(OBJ_DIR)
$(OBJ_DIR)/%.o: $(POSIX_DIR)/%.c $(wildcard $(POSIX_DIR)/*.h) $(wildcard $(SHARED_DIR)/*.h) | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDE_PATHS) -c $< -o $@

# Rule to compile shared source files into object files
# Depends on the .c file and any .h file in the shared directory
# Creates the object file in $(OBJ_DIR)
$(OBJ_DIR)/shared_%.o: $(SHARED_DIR)/%.c $(wildcard $(SHARED_DIR)/*.h) | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(INCLUDE_PATHS) -c $< -o $@

# Rule to create the build and object directories if they don't exist
# The pipe symbol '|' makes these order-only prerequisites
$(BUILD_DIR) $(OBJ_DIR):
	@mkdir -p $@

# Rule to clean up build files
clean:
	@echo "Cleaning up POSIX build files..."
	@rm -rf $(BUILD_BASE_DIR) # Remove the whole build directory

# Declare 'all' and 'clean' as phony targets (not actual files)
.PHONY: all clean
