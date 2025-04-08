#!/bin/bash

# Script to download, build, and install the Retro68 toolchain on Ubuntu,
# OR update an existing installation and rebuild.
# Clears the Retro68 source tree's InterfacesAndLibraries folder and then
# extracts 'misc/MPW_Interfaces.zip' (containing Apple's Universal
# Interfaces + added MacTCP headers) into it before building the toolchain.
#
# Installs prerequisites, clones/updates the repo, builds/rebuilds the
# toolchain in a dedicated directory within the user's home folder,
# and adds the toolchain to the PATH.

# Exit immediately if a command exits with a non-zero status.
set -e
# Treat unset variables as an error when substituting.
set -u
# Prevent errors in pipelines from being masked.
set -o pipefail

# --- Configuration ---
# Base directory for all Retro68 related files within the user's home directory
INSTALL_PARENT_DIR="$HOME"
# Directory where the Retro68 repository will be cloned
REPO_DIR="${INSTALL_PARENT_DIR}/Retro68"
# Directory where the build will take place
BUILD_DIR="${INSTALL_PARENT_DIR}/Retro68-build"
# Directory where the final toolchain will be installed (inside BUILD_DIR)
TOOLCHAIN_DIR="${BUILD_DIR}/toolchain"
# Path to the toolchain's executables
BIN_DIR="${TOOLCHAIN_DIR}/bin"
# Target directory for included interfaces/libraries within the Retro68 repo
INCLUDE_DIR_TARGET="${REPO_DIR}/InterfacesAndLibraries"
# Name of the zip file containing the Universal Interfaces
MPW_ZIP_FILE="MPW_Interfaces.zip"
# Determine the directory where this script resides
# Use BASH_SOURCE[0] which is the standard way to get the script's path
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
# Full path to the MPW interfaces zip file
MPW_ZIP_PATH="${SCRIPT_DIR}/misc/${MPW_ZIP_FILE}"

# --- Flags / Options ---
MODE="install" # Default mode

# --- Helper Functions ---
# Function to check if a command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to run commands with sudo if not already root
run_sudo() {
    if [[ $EUID -ne 0 ]]; then
        if ! command_exists sudo; then
            echo "Error: 'sudo' command not found, and script not run as root." >&2
            echo "Please install sudo or run this script as the root user." >&2
            exit 1
        fi
        echo "Running: sudo $*"
        sudo "$@"
    else
        echo "Running (as root): $*"
        "$@"
    fi
}

# Function to display usage information
usage() {
    echo "Usage: $0 [-h]"
    echo "  Installs or updates the Retro68 toolchain in ${INSTALL_PARENT_DIR}."
    echo "  Automatically extracts ${MPW_ZIP_FILE} from the script's misc directory"
    echo "  into ${INCLUDE_DIR_TARGET} if found, providing Universal Interfaces."
    echo ""
    echo "Options:"
    echo "  -h          Display this help message."
    exit 0
}


# --- Argument Parsing ---
while getopts "h" opt; do
    case ${opt} in
        h )
            usage
            ;;
        \? )
            echo "Invalid Option: -$OPTARG" 1>&2
            usage
            ;;
    esac
done
shift $((OPTIND -1))

# Check if any non-option arguments remain (we don't expect any)
if [ $# -gt 0 ]; then
    echo "Error: Unexpected arguments: $*"
    usage
fi


# --- Main Script Logic ---

echo "Starting Retro68 Toolchain Setup..."
echo "Base Directory: ${INSTALL_PARENT_DIR}"
echo "Repository: ${REPO_DIR}"
echo "Build Directory: ${BUILD_DIR}"
echo "Toolchain Binaries: ${BIN_DIR}"
echo "Looking for Universal Interfaces zip: ${MPW_ZIP_PATH}"
echo "Target for Interfaces: ${INCLUDE_DIR_TARGET}"
echo ""

# Determine if it's an install or update based on directory existence
if [ -d "$REPO_DIR" ] && [ -d "$BUILD_DIR" ]; then
    MODE="update"
    echo "Existing Retro68 directories found. Will attempt to update and rebuild."
else
    echo "No existing installation found. Performing initial setup."
fi
echo ""

# 1. Install Prerequisites (Always run this check)
echo "Step 1: Checking/Installing prerequisites..."
REQUIRED_PACKAGES=(
    build-essential # For gcc, make, etc.
    cmake
    libgmp-dev
    libmpfr-dev
    libmpc-dev
    libboost-all-dev
    bison
    flex
    texinfo
    ruby
    git
    unzip
)

# Check which packages need installing
packages_to_install=()
for pkg in "${REQUIRED_PACKAGES[@]}"; do
    if ! dpkg -s "$pkg" &> /dev/null; then
        packages_to_install+=("$pkg")
    else
        echo "  - $pkg already installed."
    fi
done

if [ ${#packages_to_install[@]} -gt 0 ]; then
    echo "The following packages need to be installed: ${packages_to_install[*]}"
    run_sudo apt-get update
    run_sudo apt-get install -y "${packages_to_install[@]}"
    echo "Prerequisites installed successfully."
else
    echo "All prerequisites are already satisfied."
fi
echo ""

# 2. Clone or Update Retro68 Repository
if [ "$MODE" == "install" ]; then
    echo "Step 2: Cloning Retro68 repository..."
    if [ -d "$REPO_DIR" ]; then
        echo "Warning: Repository directory ${REPO_DIR} already exists."
        echo "Skipping clone. If you want a fresh clone, remove ${REPO_DIR} first."
    else
        echo "Cloning into ${REPO_DIR}..."
        git clone https://github.com/autc04/Retro68.git "$REPO_DIR"
        echo "Repository cloned successfully."
    fi
    echo ""

    echo "Step 3: Initializing Git submodules..."
    cd "$REPO_DIR"
    git submodule update --init --recursive # Use recursive for nested submodules
    echo "Submodules initialized successfully."
    cd "$INSTALL_PARENT_DIR" # Navigate back
    echo ""

elif [ "$MODE" == "update" ]; then
    echo "Step 2 & 3: Updating Retro68 repository and submodules..."
    cd "$REPO_DIR"
    echo "Pulling latest changes from repository..."
    git pull
    echo "Updating submodules..."
    git submodule update --init --recursive # Ensure new submodules are fetched and all are updated
    echo "Repository and submodules updated."
    cd "$INSTALL_PARENT_DIR" # Navigate back
    echo ""
fi

# 3.5. Refresh Universal Interfaces in Retro68 source tree
echo "Step 3.5: Refreshing Universal Interfaces (${MPW_ZIP_FILE})..."
# Verify MPW_Interfaces.zip exists in the script's misc directory
if [ -f "$MPW_ZIP_PATH" ]; then
    echo "Found ${MPW_ZIP_FILE}. Refreshing ${INCLUDE_DIR_TARGET}..."

    # Ensure the target directory exists
    echo "Ensuring target directory exists: ${INCLUDE_DIR_TARGET}"
    mkdir -p "$INCLUDE_DIR_TARGET"

    # Empty the target directory's contents
    echo "Emptying existing contents of ${INCLUDE_DIR_TARGET}"
    # Use :? to prevent accidental deletion if variable is empty/unset
    # Use /* to target contents, not the directory itself
    rm -rf "${INCLUDE_DIR_TARGET:?}/"*

    # Unzip the new contents directly into the target directory
    echo "Extracting ${MPW_ZIP_FILE} into ${INCLUDE_DIR_TARGET}..."
    if unzip -o "$MPW_ZIP_PATH" -d "$INCLUDE_DIR_TARGET"; then
        echo "${MPW_ZIP_FILE} extracted successfully into ${INCLUDE_DIR_TARGET}."
    else
        echo "Error: Failed to extract ${MPW_ZIP_FILE} into ${INCLUDE_DIR_TARGET}." >&2
        exit 1
    fi
else
    echo "Warning: ${MPW_ZIP_FILE} not found in script's misc directory (${MPW_ZIP_PATH})."
    echo "Retro68 will be built using only the default Multiversal Interfaces."
    # Ensure the target directory exists even if empty, as Retro68 might expect it
    mkdir -p "$INCLUDE_DIR_TARGET"
fi
echo ""


# 4. Create Build Directory (only if installing)
if [ "$MODE" == "install" ]; then
    echo "Step 4: Creating build directory..."
    if [ -d "$BUILD_DIR" ]; then
        echo "Warning: Build directory ${BUILD_DIR} already exists."
        echo "Consider removing it for a completely clean build: rm -rf ${BUILD_DIR}"
    else
        mkdir -p "$BUILD_DIR"
        echo "Build directory ${BUILD_DIR} created."
    fi
    echo ""
fi

# 5. Build or Rebuild the Toolchain
if [ "$MODE" == "install" ]; then
    echo "Step 5: Building the toolchain (this may take a long time)..."
elif [ "$MODE" == "update" ]; then
    echo "Step 5: Rebuilding the toolchain with updates (this may take a long time)..."
fi
echo "Build logs will be inside ${BUILD_DIR}"
# Navigate into the build directory to run the build script
cd "$BUILD_DIR"
# Execute the build script located in the repository directory
# This script should handle incremental builds, but running it ensures
# everything is processed after updates.
if ! "${REPO_DIR}/build-toolchain.bash"; then
    echo "Error: Toolchain build failed. Check logs in ${BUILD_DIR}" >&2
    exit 1
fi
echo "Toolchain build completed successfully."
# Navigate back
cd "$INSTALL_PARENT_DIR"
echo ""

# 6. Update PATH (Always check this)
echo "Step 6: Checking/Adding toolchain to PATH in ~/.bashrc..."
BASHRC_FILE="${HOME}/.bashrc"
PATH_EXPORT_LINE="export PATH=\"${BIN_DIR}:\$PATH\"" # Note the escaped $PATH

if [ -f "$BASHRC_FILE" ]; then
    # Check if the exact line is already present
    if grep -qF "${PATH_EXPORT_LINE}" "$BASHRC_FILE"; then
        echo "Toolchain path already found in ${BASHRC_FILE}. Skipping."
    else
        echo "Adding PATH export to ${BASHRC_FILE}..."
        # Append the line to .bashrc
        echo "" >> "$BASHRC_FILE" # Add a newline for separation
        echo "# Add Retro68 toolchain to PATH" >> "$BASHRC_FILE"
        echo "${PATH_EXPORT_LINE}" >> "$BASHRC_FILE"
        echo "PATH added."
        echo ""
        echo "------------------------------------------------------------------"
        # This is the line the user suspected (around original line 273)
        # It looks syntactically correct.
        echo "IMPORTANT: To use the Retro68 commands (like m68k-apple-macos-gcc),"
        echo "you need to reload your shell configuration or open a new terminal."
        echo "You can do this now by running:"
        echo "  source ${BASHRC_FILE}"
        echo "------------------------------------------------------------------"
    fi
else
    echo "Warning: ${BASHRC_FILE} not found. Could not automatically add toolchain to PATH."
    echo "Please add the following line manually to your shell configuration file:"
    echo "  ${PATH_EXPORT_LINE}"
fi
echo ""

echo "Retro68 setup/update complete!"
echo "Toolchain is located in: ${TOOLCHAIN_DIR}"
