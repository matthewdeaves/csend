#!/bin/bash

# convert_to_mac.sh
# Converts specified text files or all relevant text files within specified directories
# from a source encoding (default UTF-8) with LF or CRLF line endings
# to Mac Roman encoding with Classic Mac CR line endings.
# Outputs converted files into a dedicated directory.

# --- Configuration ---
SOURCE_ENCODING="UTF-8"
TARGET_ENCODING="MACROMAN"
OUTPUT_DIR="mac_output" # Name of the directory to store converted files
MAX_FILENAME_LEN=31      # Classic Mac HFS filename limit

# Define patterns for files to convert when processing directories
# Add or remove patterns as needed (e.g., ".p" for Pascal, ".a" for Assembly)
# Note: Case-insensitive matching used later.
FILE_PATTERNS=( "*.c" "*.h" "*.r" "Makefile" )

# --- Check for dependencies ---
COMMANDS_NEEDED=("iconv" "tr" "find" "mkdir" "basename" "dirname")
MISSING_CMDS=0
for cmd in "${COMMANDS_NEEDED[@]}"; do
    if ! command -v "$cmd" &> /dev/null; then
        echo "Error: Required command '$cmd' not found. Please install it."
        MISSING_CMDS=1
    fi
done
if [ "$MISSING_CMDS" -eq 1 ]; then
    exit 1
fi

# --- Usage ---
if [ "$#" -eq 0 ]; then
    echo "Usage: $0 <file_or_dir1> [file_or_dir2] ..."
    echo "  Converts specified text files or relevant files within specified directories"
    echo "  from $SOURCE_ENCODING (LF/CRLF) to $TARGET_ENCODING (CR)."
    echo "  Relevant files match patterns: ${FILE_PATTERNS[*]}"
    echo "  Output files are placed in the '$OUTPUT_DIR/' directory."
    echo "  Warns if output filenames exceed $MAX_FILENAME_LEN characters."
    exit 1
fi

# --- Prepare Output Directory ---
# Use mkdir -p to create the directory and ignore errors if it already exists
# Also creates parent directories if needed.
if ! mkdir -p "$OUTPUT_DIR"; then
    echo "Error: Failed to create output directory '$OUTPUT_DIR'."
    exit 1
fi
echo "Output directory: '$OUTPUT_DIR'"

# --- Function to process a single file ---
process_file() {
    local infile="$1"
    local fname
    local outfile

    # Check if it's a regular file (and not a directory passed erroneously)
    if [ ! -f "$infile" ]; then
         echo "Warning: '$infile' is not a regular file. Skipping."
         return 1
    fi

    # Get the base filename
    fname=$(basename "$infile")

    # Check filename length
    if [ ${#fname} -gt $MAX_FILENAME_LEN ]; then
        echo "Warning: Filename '$fname' (from '$infile') exceeds $MAX_FILENAME_LEN characters."
    fi

    # Define the output path inside the output directory
    outfile="$OUTPUT_DIR/$fname"

    echo "Processing '$infile' -> '$outfile'..."

    # --- Perform Conversion ---
    # 1. Convert encoding from Source to Target (e.g., UTF-8 to MacRoman) using iconv
    # 2. Pipe to tr to handle line endings:
    #    a) delete any existing CR ('\r') to handle CRLF/CR input gracefully
    #    b) translate LF ('\n') to CR ('\r')
    if iconv -f "$SOURCE_ENCODING" -t "$TARGET_ENCODING" "$infile" | tr -d '\r' | tr '\n' '\r' > "$outfile"; then
        echo "  Successfully converted '$infile'."
        return 0 # Indicate success
    else
        echo "  Error converting '$infile'. Check 'iconv' compatibility or file content."
        # Clean up partially created outfile on error
        rm -f "$outfile"
        return 1 # Indicate failure
    fi
}

# --- Process Input Arguments (Files or Directories) ---
EXIT_CODE=0
for argument in "$@"; do
    if [ -f "$argument" ]; then
        # It's a file, process it directly
        if ! process_file "$argument"; then
            EXIT_CODE=1 # Record failure
        fi
    elif [ -d "$argument" ]; then
        # It's a directory, find and process relevant files within it
        echo "Scanning directory '$argument'..."
        # Build the find command arguments for name patterns
        find_args=()
        first_pattern=1
        for pattern in "${FILE_PATTERNS[@]}"; do
            if [ "$first_pattern" -eq 1 ]; then
                find_args+=( "-iname" "$pattern" ) # Use -iname for case-insensitivity
                first_pattern=0
            else
                find_args+=( "-o" "-iname" "$pattern" )
            fi
        done

        # Use find to locate files matching the patterns (top-level only for simplicity here)
        # Use -print0 and read -d '' for safer handling of filenames with odd characters
        # Use maxdepth 1 to avoid recursion unless needed. Remove it for recursive search.
        find "$argument" -maxdepth 1 -type f \( "${find_args[@]}" \) -print0 | while IFS= read -r -d $'\0' found_file; do
             if ! process_file "$found_file"; then
                 EXIT_CODE=1 # Record failure
             fi
        done
        # Check find's exit status (optional, find itself usually reports errors)
        # find_status=$?
        # if [ $find_status -ne 0 ]; then
        #     echo "Warning: 'find' command encountered issues in '$argument'."
        #     EXIT_CODE=1
        # fi

    else
        echo "Warning: Argument '$argument' is neither a file nor a directory. Skipping."
    fi
done

echo "Conversion process finished."
exit $EXIT_CODE # Exit with 0 if all successful, 1 otherwise

