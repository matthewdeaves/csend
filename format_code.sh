#!/bin/bash

# Script to format C/C++ source files using Astyle

# --- Configuration ---
# Astyle options for formatting.
# --style=kr: K&R style - opening braces on the same line for function definitions,
#             and often for other blocks like if/else/while (configurable with other options).
# --indent=spaces=4: Indent with 4 spaces
# --convert-tabs: Convert tabs to spaces
# --align-pointer=name: Align pointer/reference to the variable name (e.g., char* p)
# --pad-oper: Pad operators with spaces (e.g., a = b)
# --unpad-paren: Remove extra spaces inside parentheses
# --keep-one-line-blocks: If a block is a single line, keep it as such
#                         e.g. if (foo) { bar(); }
# --pad-header: Puts a space between `if`/`for`/`while` and the opening parenthesis.
#               e.g. `if (condition)` rather than `if(condition)`
# --suffix=none: Overwrite original files (no .orig backup)
# --lineend=linux: Ensure Linux-style line endings (LF)
# -q: Quiet mode (optional, remove for more verbosity)

# For function definitions like: void foo() {
# And other blocks like: if (condition) {
# Astyle's K&R style generally handles this.
# We might need --attach-closing-while if you use do-while loops and want the `while` attached to the `}`.
ASTYLE_OPTIONS="--style=kr --indent=spaces=4 --convert-tabs --align-pointer=name --pad-oper --unpad-paren --pad-header --keep-one-line-blocks --suffix=none --lineend=linux -q"

# Directories to exclude from formatting
EXCLUDE_DIRS=("./build" "./obj" "./tools")

# --- Check for Astyle ---
if ! command -v astyle &> /dev/null; then
    echo "Artistic Style (astyle) could not be found."
    echo "Please install it to continue."
    echo ""
    echo "Installation instructions:"
    echo "  Debian/Ubuntu: sudo apt-get update && sudo apt-get install astyle"
    echo "  Fedora:        sudo dnf install astyle"
    echo "  macOS (Homebrew): brew install astyle"
    echo "  Other:         Check your package manager or visit http://astyle.sourceforge.net/"
    exit 1
fi

echo "Found Astyle: $(astyle --version)"
echo ""
echo "This script will format all .c and .h files in the current project directory"
echo "and its subdirectories, overwriting the original files."
echo "Excluded directories: ${EXCLUDE_DIRS[*]}"
echo ""
echo "Chosen Astyle options: ${ASTYLE_OPTIONS}"
echo ""
echo "MAKE SURE YOUR CHANGES ARE COMMITTED TO GIT BEFORE PROCEEDING!"
echo ""

read -r -p "Are you sure you want to format the files? (yes/no): " confirm

if [[ "${confirm,,}" != "yes" ]]; then
    echo "Formatting cancelled by user."
    exit 0
fi

echo ""
echo "Formatting files..."

# Prepare find command exclusions
find_exclude_opts=()
for exclude_dir in "${EXCLUDE_DIRS[@]}"; do
    if [ -d "$exclude_dir" ]; then # Only add prune if directory exists
        find_exclude_opts+=(-path "$exclude_dir" -prune -o)
    fi
done

# Find and format files
formatted_count=0
error_count=0

while IFS= read -r -d $'\0' file; do
    echo "Formatting: $file"
    if astyle ${ASTYLE_OPTIONS} "$file"; then
        ((formatted_count++))
    else
        echo "Error formatting $file"
        ((error_count++))
    fi
done < <(find . \
    "${find_exclude_opts[@]}" \
    -type f \( -name "*.c" -o -name "*.h" \) -print0)

echo ""
echo "--- Formatting Complete ---"
echo "Files formatted: $formatted_count"
if [ "$error_count" -gt 0 ]; then
    echo "Errors encountered: $error_count"
fi
echo "Please review the changes with 'git diff' before committing."