#!/bin/bash

# Script to check for code duplication using PMD's CPD (Copy/Paste Detector)

# --- Configuration ---
PMD_VERSION="6.55.0"
MIN_TOKENS=35
# Directories containing the C source code to scan (space-separated)
SOURCE_DIRS="posix shared classic_mac_mactcp classic_mac_ot"
# Patterns for directories TO EXCLUDE. Use find's predicates, as array elements.
# Use '*/<dirname>' to match the directory name anywhere.
EXCLUDE_PATTERNS=( -path '*/.finf' -o -path '*/.rsrc' )
LANGUAGE="c"
# File matching expression array for find. Separate elements for find syntax.
# Parentheses must be separate arguments. Use single quotes around * glob.
FILE_MATCHING_EXPR=( '(' -name '*.c' -not -path '*/DNR.c' ')' )
# Example for C and H files:
# FILE_MATCHING_EXPR=( '(' -name '*.c' -o -name '*.h' ')' )
# Reports base directory
REPORTS_BASE_DIR="reports/cpd"
# Timestamp for this run
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
# Reports directory for this run
REPORTS_DIR="${REPORTS_BASE_DIR}/${TIMESTAMP}"

# --- Internal Variables ---
TOOLS_DIR="tools"
PMD_DIST_NAME="pmd-bin-${PMD_VERSION}"
PMD_ZIP="${PMD_DIST_NAME}.zip"
PMD_URL="https://github.com/pmd/pmd/releases/download/pmd_releases%2F${PMD_VERSION}/${PMD_ZIP}"
PMD_HOME="${TOOLS_DIR}/${PMD_DIST_NAME}"
CPD_RUNNER="" # Initialize runner path as empty
# Use a more descriptive temp file name if possible (or keep random)
FILE_LIST_TMP=$(mktemp --tmpdir cpd-filelist.XXXXXX) || { echo "Failed to create temp file"; exit 1; }

# --- Helper Functions ---
check_command() {
    if ! command -v "$1" &> /dev/null; then
        echo "Error: Required command '$1' not found."
        echo "Please install '$1' (e.g., using 'sudo apt update && sudo apt install $1')."
        # Clean up temp file on error exit
        rm -f "$FILE_LIST_TMP"
        exit 1
    fi
}

# Cleanup function to remove temp file on exit
cleanup() {
    # echo "Cleaning up ${FILE_LIST_TMP}..." # Optional debug
    rm -f "$FILE_LIST_TMP"
}
# Register cleanup function to run on script exit (normal or error)
trap cleanup EXIT SIGINT SIGTERM

# --- Main Logic ---

# Create reports directory
mkdir -p "$REPORTS_DIR"

# Count existing reports
if [ -d "$REPORTS_BASE_DIR" ]; then
    EXISTING_COUNT=$(find "$REPORTS_BASE_DIR" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)
    if [ "$EXISTING_COUNT" -gt 1 ]; then
        echo "Found $((EXISTING_COUNT - 1)) existing CPD report(s), creating new report for ${TIMESTAMP}"
    fi
fi
echo "Report directory: ${REPORTS_DIR}"
echo ""

# Check for Java first
if ! command -v java &> /dev/null; then
    echo "Error: Java is required to run PMD/CPD but was not found."
    echo ""
    echo "Please install Java using one of these commands:"
    echo "  Ubuntu/Debian:  sudo apt update && sudo apt install default-jre"
    echo "  Fedora:         sudo dnf install java-latest-openjdk"
    echo "  macOS:          brew install openjdk"
    echo ""
    echo "After installation, you may need to restart your terminal or run:"
    echo "  source ~/.bashrc"
    exit 1
fi

# 1. Check Dependencies
check_command "wget"
check_command "unzip"
check_command "find"
check_command "mktemp"
check_command "wc"

# 2. Check/Download/Extract PMD
if [ ! -d "$PMD_HOME" ]; then
    echo "PMD version ${PMD_VERSION} not found in ${TOOLS_DIR}."
    echo "Attempting to download and set up PMD..."
    mkdir -p "$TOOLS_DIR"
    if [ $? -ne 0 ]; then echo "Error: Failed to create directory ${TOOLS_DIR}"; exit 1; fi
    echo "Downloading PMD from ${PMD_URL}..."
    wget --progress=dot:giga -O "${TOOLS_DIR}/${PMD_ZIP}" "$PMD_URL"
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


# 4. Generate file list using find, excluding specified patterns
echo "Generating list of files to scan..."
echo "Scanning source directories: ${SOURCE_DIRS}"
echo "Excluding items matching: ${EXCLUDE_PATTERNS[@]}" # Note: These are find conditions
echo "Including file types matching expression: ${FILE_MATCHING_EXPR[@]}" # Note: These are find conditions

# Clear the temp file first
> "$FILE_LIST_TMP"

# CONSTRUCT THE FIND COMMAND ARRAY
FIND_CMD=( find )
# Add source directories robustly by splitting the string
read -r -a source_dirs_array <<< "$SOURCE_DIRS"
FIND_CMD+=( "${source_dirs_array[@]}" )
# Add pruning logic: Use literal parentheses arguments around the exclude patterns
# Format: \( <exclude_conditions...> \) -prune
 FIND_CMD+=( '(' "${EXCLUDE_PATTERNS[@]}" ')' -prune )
# Add find logic for non-pruned items: -o \( <include_conditions...> \) -type f -print
FIND_CMD+=( -o "${FILE_MATCHING_EXPR[@]}" -type f -print )

# Execute the find command directly, redirecting output
echo "Executing find command: ${FIND_CMD[@]}" # Debug output
if ! "${FIND_CMD[@]}" >> "$FILE_LIST_TMP"; then
    find_exit_code=$?
    echo "Error: 'find' command failed with exit code $find_exit_code."
    exit 1
fi


file_count=$(wc -l < "$FILE_LIST_TMP")
echo "Found ${file_count} files to scan."

# DEBUG: Check the actual contents of the file list if count seems wrong or errors persist
# Can be useful if find command looks right but still includes wrong files.
# echo "--- BEGIN File List Contents (${FILE_LIST_TMP}) ---"
# cat "${FILE_LIST_TMP}"
# echo "--- END File List Contents ---"

if [ "$file_count" -eq 0 ]; then
    echo "Warning: No source files found matching the criteria. Check SOURCE_DIRS, EXCLUDE_PATTERNS, and FILE_MATCHING_EXPR."
    exit 0 # Exit cleanly, nothing to scan
fi


# 5. Prepare CPD command arguments using --file-list (Fixed)
cmd_args=()
cmd_args+=( "$CPD_RUNNER" )
if [[ "$CPD_RUNNER" == *run.sh ]]; then
    cmd_args+=( "cpd" )
fi
cmd_args+=(
    "--language" "$LANGUAGE"
    "--minimum-tokens" "$MIN_TOKENS"
    "--format" "text"
    "--file-list" "$FILE_LIST_TMP" # Use the generated file list - CORRECTED OPTION
)

# 6. Run CPD
echo "-----------------------------------------------------"
echo "Running CPD (Copy/Paste Detector)..."
echo "Minimum token length: ${MIN_TOKENS}"
echo "Language: ${LANGUAGE}"
echo "Using runner: ${CPD_RUNNER}"
echo "Using file list: ${FILE_LIST_TMP} (${file_count} files)"
# echo "Full command (DEBUG): ${cmd_args[@]}"
echo "-----------------------------------------------------"

# Execute the assembled CPD command and save to file
CPD_OUTPUT="$REPORTS_DIR/cpd_results.txt"
"${cmd_args[@]}" 2>&1 | tee "$CPD_OUTPUT"
cpd_exit_code=$?
# Capture CPD exit code immediately


echo "-----------------------------------------------------"
# Generate LLM summary
LLM_SUMMARY="$REPORTS_DIR/llm_summary.txt"

# Count duplicate blocks from output (more reliable than exit code)
DUPE_COUNT=$(grep -c "^Found a" "$CPD_OUTPUT" 2>/dev/null || echo 0)

{
    echo "CODE DUPLICATION ANALYSIS - LLM SUMMARY"
    echo "======================================="
    echo "Generated: $(date)"
    echo "Minimum Tokens: ${MIN_TOKENS}"
    echo "Files Scanned: ${file_count}"
    echo ""

    if [ "$DUPE_COUNT" -eq 0 ]; then
        echo "## RESULT: ✓ NO SIGNIFICANT DUPLICATION FOUND"
        echo ""
        echo "No code duplication detected above the minimum token threshold of ${MIN_TOKENS}."
        echo "This indicates good code organization with minimal copy-paste patterns."
    else
        echo "## RESULT: ⚠ CODE DUPLICATION DETECTED"
        echo ""

        echo "Number of duplicate blocks: $DUPE_COUNT"
        echo ""

        # Extract duplications with context
        echo "## DUPLICATED CODE BLOCKS"
        echo ""
        grep -A 20 "^Found a" "$CPD_OUTPUT" 2>/dev/null | head -100
        echo ""
        echo "## RECOMMENDATIONS"
        echo ""
        echo "1. Review duplicate blocks above"
        echo "2. Consider extracting common code into shared functions"
        echo "3. Evaluate if duplication is intentional (e.g., platform-specific implementations)"
        echo "4. Reduce technical debt by consolidating duplicate logic"
    fi

    if [ $cpd_exit_code -ne 0 ] && [ $cpd_exit_code -ne 4 ]; then
        echo "## RESULT: ⚠ ANALYSIS ERROR"
        echo ""
        echo "CPD finished with exit code: $cpd_exit_code"
        echo "This may indicate parsing errors or configuration issues."
        echo "Check cpd_results.txt for detailed error messages."
    fi

    echo ""
    echo "## FILES ANALYZED"
    echo ""
    echo "Source directories: ${SOURCE_DIRS}"
    echo "Excluded patterns: ${EXCLUDE_PATTERNS[@]}"
    echo ""
    echo "For detailed output, see: cpd_results.txt"

} > "$LLM_SUMMARY"

if [ "$DUPE_COUNT" -eq 0 ]; then
    echo "✓ CPD analysis complete. No significant code duplication found (above the minimum token limit)."
elif [ "$DUPE_COUNT" -gt 0 ]; then
    echo "⚠ CPD analysis complete. Found $DUPE_COUNT duplicate code block(s)."
fi

if [ $cpd_exit_code -ne 0 ] && [ $cpd_exit_code -ne 4 ]; then
    # Any other non-zero exit code usually indicates an error during analysis
    echo "⚠ CPD analysis finished with an unexpected exit code: $cpd_exit_code."
    echo "There might have been an error during analysis (e.g., parsing errors, configuration issues)."
    echo "Check the output above for details like SEVERE messages or stack traces."
fi
echo "-----------------------------------------------------"
echo ""
echo "Reports saved to: $REPORTS_DIR/"
echo "  - cpd_results.txt  : Full CPD output"
echo "  - llm_summary.txt  : LLM-optimized summary"
echo ""
echo "💡 LLM Summary: $REPORTS_DIR/llm_summary.txt"

# Exit with the CPD exit code (0=ok, 4=violations found, other=error)
# Cleanup happens automatically via trap EXIT
exit $cpd_exit_code
