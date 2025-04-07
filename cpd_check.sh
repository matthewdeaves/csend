#!/bin/bash

# Script to check for code duplication using PMD's CPD (Copy/Paste Detector)

# --- Configuration ---
PMD_VERSION="6.55.0"
MIN_TOKENS=50
# Directories containing the C source code to scan (space-separated)
SOURCE_DIRS="posix shared classic_mac"
LANGUAGE="c"

# --- Internal Variables ---
TOOLS_DIR="tools"
PMD_DIST_NAME="pmd-bin-${PMD_VERSION}"
PMD_ZIP="${PMD_DIST_NAME}.zip"
PMD_URL="https://github.com/pmd/pmd/releases/download/pmd_releases%2F${PMD_VERSION}/${PMD_ZIP}"
PMD_HOME="${TOOLS_DIR}/${PMD_DIST_NAME}"
CPD_RUNNER="" # Initialize runner path as empty

# --- Helper Functions ---
check_command() {
    if ! command -v "$1" &> /dev/null; then
        echo "Error: Required command '$1' not found."
        echo "Please install '$1' (e.g., using 'sudo apt update && sudo apt install $1')."
        exit 1
    fi
}

# --- Main Logic ---

# 1. Check Dependencies
check_command "wget"
check_command "unzip"

# 2. Check/Download/Extract PMD
if [ ! -d "$PMD_HOME" ]; then
    echo "PMD version ${PMD_VERSION} not found in ${TOOLS_DIR}."
    echo "Attempting to download and set up PMD..."
    mkdir -p "$TOOLS_DIR"
    if [ $? -ne 0 ]; then echo "Error: Failed to create directory ${TOOLS_DIR}"; exit 1; fi
    echo "Downloading PMD from ${PMD_URL}..."
    wget -O "${TOOLS_DIR}/${PMD_ZIP}" "$PMD_URL"
    if [ $? -ne 0 ]; then echo "Error: Failed to download PMD."; rm -f "${TOOLS_DIR}/${PMD_ZIP}"; exit 1; fi
    echo "Extracting PMD..."
    unzip -q "${TOOLS_DIR}/${PMD_ZIP}" -d "$TOOLS_DIR"
    if [ $? -ne 0 ]; then echo "Error: Failed to extract PMD."; rm -f "${TOOLS_DIR}/${PMD_ZIP}"; rm -rf "$PMD_HOME"; exit 1; fi
    echo "Cleaning up downloaded zip file..."
    rm "${TOOLS_DIR}/${PMD_ZIP}"
    echo "PMD setup complete in ${PMD_HOME}"
else
    echo "Found existing PMD installation in ${PMD_HOME}."
fi

# 3. Verify the CPD runner script exists and set CPD_RUNNER path
echo "Verifying PMD runner script..."
PMD6_RUNNER_SCRIPT="${PMD_HOME}/bin/run.sh"
PMD7_RUNNER_SCRIPT="${PMD_HOME}/bin/cpd"

if [ -f "$PMD6_RUNNER_SCRIPT" ] && [ -x "$PMD6_RUNNER_SCRIPT" ]; then
    echo "Found PMD 6.x runner: ${PMD6_RUNNER_SCRIPT}"
    CPD_RUNNER="$PMD6_RUNNER_SCRIPT"
elif [ -f "$PMD7_RUNNER_SCRIPT" ] && [ -x "$PMD7_RUNNER_SCRIPT" ]; then
    echo "Found PMD 7+ runner: ${PMD7_RUNNER_SCRIPT}"
    CPD_RUNNER="$PMD7_RUNNER_SCRIPT"
else
    echo "Error: Could not find a valid PMD runner script."
    echo "Checked for executable files at:"
    echo "  - ${PMD6_RUNNER_SCRIPT} (for PMD 6.x)"
    echo "  - ${PMD7_RUNNER_SCRIPT} (for PMD 7+)"
    echo "Please check the contents of the '${PMD_HOME}/bin/' directory manually."
    echo "Listing contents of ${PMD_HOME}/bin/:"
    ls -l "${PMD_HOME}/bin/"
    exit 1
fi

# 4. Prepare CPD arguments (convert spaces to commas for PMD 6.x --files)
CPD_FILES_ARG=$(echo "$SOURCE_DIRS" | tr ' ' ',')

# 5. Run CPD
echo "-----------------------------------------------------"
echo "Running CPD (Copy/Paste Detector)..."
echo "Minimum token length: ${MIN_TOKENS}"
echo "Scanning directories: ${SOURCE_DIRS} (as ${CPD_FILES_ARG})" # Show both formats
echo "Language: ${LANGUAGE}"
echo "Using runner: ${CPD_RUNNER}"
echo "-----------------------------------------------------"

# Execute CPD command based on which runner was found
if [[ "$CPD_RUNNER" == *run.sh ]]; then
    # PMD 6.x command structure - use comma-separated list for --files
    "$CPD_RUNNER" cpd --language "$LANGUAGE" --minimum-tokens "$MIN_TOKENS" --files "$CPD_FILES_ARG" --format text
else
    # Assume PMD 7+ command structure - use space-separated list for --files (or potentially --dir multiple times)
    # For simplicity, we'll try --files with space separation here, which *might* work in PMD 7
    # A more robust PMD 7 approach might loop and use --dir for each directory.
    "$CPD_RUNNER" --language "$LANGUAGE" --minimum-tokens "$MIN_TOKENS" --files "$SOURCE_DIRS" --format text
fi

cpd_exit_code=$?

echo "-----------------------------------------------------"
if [ $cpd_exit_code -eq 0 ]; then
    echo "CPD analysis complete. No significant code duplication found."
elif [ $cpd_exit_code -eq 4 ]; then
    echo "CPD analysis complete. Code duplication found (see details above)."
else
    echo "CPD analysis finished with an unexpected exit code: $cpd_exit_code."
    echo "There might have been an error during analysis."
fi
echo "-----------------------------------------------------"

exit $cpd_exit_code
