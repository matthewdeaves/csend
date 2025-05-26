#!/bin/bash

# codemerge.sh - A tool to merge source files for AI assistance and other purposes
# Usage: ./codemerge.sh [options]

# Default values
output_file="combined_code.txt"
include_headers=true
platform="all" # Default platform selection
add_separator=true
separator_line="//===================================="
include_makefile=false
include_docker=false
include_tree=true # Default: include tree output
strip_comments=false # Default: don't strip comments
gcc_available=false # Flag to check if gcc is present (needed for stripping)

# Display help message
show_help() {
    echo "Usage: $0 [options]"
    echo
    echo "Merges source code from shared/, posix/, and classic_mac/ directories."
    echo "Includes project structure via 'tree .' at the beginning by default."
    echo "Includes .r resource files from classic_mac/."
    echo
    echo "Options:"
    echo "  -o, --output FILE          Specify output file (default: combined_code.txt)"
    echo "  -P, --platform PLATFORM    Specify platform: posix, classic, all (default: all)"
    echo "  -n, --no-headers           Don't include filename headers"
    echo "  -m, --no-separators        Don't include separator lines between files"
    echo "  -M, --include-makefile     Append root Makefile and Makefile.retro68 content at the end"
    echo "  -D, --include-docker       Append root Dockerfile, docker-compose.yml and docker.sh content at the end"
    echo "  -T, --no-tree              Don't include 'tree .' output at the beginning"
    echo "  -S, --strip-comments       Remove C/C++ comments (//, /* */) from .c and .h files (requires gcc)"
    echo "  -h, --help                 Display this help message"
    echo
    echo "Examples:"
    echo "  $0 -o posix_for_ai.txt -P posix -M -S"
    echo "  $0 --platform classic --no-separators --no-tree"
    echo "  $0 -o project_snapshot.txt # Defaults to --platform all and includes tree"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -o|--output)
            output_file="$2"
            shift 2
            ;;
        -P|--platform)
            platform_arg=$(echo "$2" | tr '[:upper:]' '[:lower:]') # Convert to lowercase
            if [[ "$platform_arg" == "posix" || "$platform_arg" == "classic" || "$platform_arg" == "all" ]]; then
                platform="$platform_arg"
            else
                echo "Error: Invalid platform '$2'. Use 'posix', 'classic', or 'all'."
                exit 1
            fi
            shift 2
            ;;
        -n|--no-headers)
            include_headers=false
            shift
            ;;
        -m|--no-separators)
            add_separator=false
            shift
            ;;
        -M|--include-makefile)
            include_makefile=true
            shift
            ;;
        -D|--include-docker)
            include_docker=true
            shift
            ;;
        -T|--no-tree)
            include_tree=false
            shift
            ;;
        -S|--strip-comments) # New option
            strip_comments=true
            shift
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

# Check for gcc if stripping is requested
if [ "$strip_comments" = true ]; then
    if command -v gcc &> /dev/null; then
        gcc_available=true
        echo "Info: Comment stripping enabled for .c/.h files (using gcc)."
    else
        echo "Error: --strip-comments requires 'gcc' but it was not found."
        echo "Please install gcc or run without --strip-comments."
        exit 1
    fi
fi


# Create or clear the output file
> "$output_file"

# Function to append a file with optional header/separator/comment stripping
# Takes full path as $1, uses it to generate relative path for header
append_file_content() {
    local file_to_append="$1"
    local output_target="$2"
    local use_headers="$3"
    local use_separator="$4"
    local sep_line="$5"
    # Access global: strip_comments, gcc_available

    local header_prefix="//"
    local file_basename=$(basename "$file_to_append")

    # Guess comment style based on extension for header
    if [[ "$file_basename" == *Makefile* || "$file_basename" == *Dockerfile* || "$file_basename" == *docker-compose.yml* || "$file_basename" == *.sh ]]; then
        header_prefix="#"
    elif [[ "$file_basename" == *.r ]]; then
        header_prefix="//" # Rez files often use C/C++ style comments
    fi


    if [ -f "$file_to_append" ]; then
        echo "Appending $file_to_append..."
        if [ "$use_headers" = true ]; then
            if [ -s "$output_target" ]; then
                 echo -e "\n$sep_line" >> "$output_target"
            else
                 echo "$sep_line" >> "$output_target"
            fi
            echo "$header_prefix FILE: ./$file_to_append" >> "$output_target"
            echo -e "$sep_line\n" >> "$output_target"
        elif [ "$use_separator" = true ]; then
             if [ -s "$output_target" ]; then
                 echo -e "\n--- $file_to_append ---\n" >> "$output_target"
             else
                 echo -e "--- $file_to_append ---\n" >> "$output_target"
             fi
        fi

        # Check if stripping is enabled globally, gcc is available, and file is .c or .h
        if [ "$strip_comments" = true ] && [ "$gcc_available" = true ] && [[ "$file_to_append" == *.c || "$file_to_append" == *.h ]]; then
            echo "Stripping comments from $file_to_append..."
            # Use gcc preprocessor to remove comments, keeping defines, avoiding includes
            # -fpreprocessed: Treat input as already preprocessed (helps avoid expanding system includes)
            # -dD: Keep macro definitions (#define) but don't expand them
            # -E: Run preprocessor only
            # Redirect stderr to /dev/null to hide potential gcc warnings unless it fails
            if ! gcc -fpreprocessed -dD -E "$file_to_append" >> "$output_target" 2>/dev/null; then
                local gcc_exit_code=$?
                echo "Warning: Failed to strip comments from $file_to_append using gcc (exit code $gcc_exit_code). Appending original file."
                # Fallback to cat if gcc fails
                cat "$file_to_append" >> "$output_target"
            fi
        else
            # Append normally for other files or if stripping is off/gcc unavailable
            cat "$file_to_append" >> "$output_target"
        fi

        # Add extra newline for spacing after content, only if separators are enabled AND headers are off
        if [ "$use_separator" = true ] && [ "$use_headers" = false ]; then
             echo "" >> "$output_target"
        fi
        return 0 # Success
    else
        echo "Warning: File '$file_to_append' not found, skipping."
        return 1 # Failure
    fi
}

# Function to process specific file types (.h, .c) in a given directory
process_hc_directory() {
    local dir="$1"
    local output_target="$2"
    local use_headers="$3"
    local use_separator="$4"
    local sep_line="$5"
    local processed=0

    if [ ! -d "$dir" ]; then
        echo "Info: Directory '$dir' does not exist, skipping."
        return 0
    fi

    echo "Processing C/H files in directory: $dir"

    # Process header files first, sorted alphabetically
    while IFS= read -r file; do
        if [ -f "$file" ]; then
            # Pass arguments to append_file_content (it accesses globals for stripping)
            append_file_content "$file" "$output_target" "$use_headers" "$use_separator" "$sep_line"
             ((processed++))
        fi
    done < <(find "$dir" -maxdepth 1 -name '*.h' 2>/dev/null | sort)

    # Process source files, sorted alphabetically
    while IFS= read -r file; do
        if [ -f "$file" ]; then
            # Pass arguments to append_file_content (it accesses globals for stripping)
            append_file_content "$file" "$output_target" "$use_headers" "$use_separator" "$sep_line"
            ((processed++))
        fi
    done < <(find "$dir" -maxdepth 1 -name '*.c' 2>/dev/null | sort)

    return $processed
}


# --- Main Processing ---

echo "Starting code merge..."
echo "Output file: $output_file"
echo "Platform(s): $platform"
if [ "$strip_comments" = true ]; then echo "Stripping comments from .c/.h: Yes (requires gcc)"; fi # Added status

# 0. Add Tree Output if requested
tree_output_added=false
if [ "$include_tree" = true ]; then
    # Construct the ignore pattern dynamically to include the output file name and exclude .sh files
    ignore_pattern="build|obj|tools|misc|csend_venv|doxygen_docs|images|logs|__pycache__|Books|*.md|*.sh|$output_file"

    if command -v tree &> /dev/null; then
        echo "Adding project structure (tree .)..."
        {
            echo "#===================================="
            echo "# Project Structure (tree .)"
            echo "# Excluded: build/, obj/, tools/, misc/, *.md, *.sh, $output_file"
            echo "#===================================="

            # Use the dynamically constructed ignore pattern
            if ! tree_output=$(tree -I "$ignore_pattern" . 2>/dev/null) || [ -z "$tree_output" ]; then
                echo "# Tree command failed or produced no output. Using simple directory listing:"
                echo "# Main directories (excluding build, obj, tools, misc):"
                find . -type d -maxdepth 1 -not -path "*/\.*" -not -path "./build" -not -path "./obj" -not -path "./tools" -not -path "./misc" -not -path "./Books" -not -path "./images" -not -path "./doxygen_docs" | sort
                echo "# ----------------"
                echo "# Files in root (excluding .md, .sh, $output_file):"
                find . -type f -maxdepth 1 -not -path "*/\.*" -not -name '*.md' -not -name '*.sh' -not -name "$output_file" | sort
            else
                echo "$tree_output"
            fi

            echo -e "\n#====================================\n"
        } >> "$output_file"
        tree_output_added=true
    else
        echo "Warning: 'tree' command not found. Using simple directory listing instead."
        {
            echo "#===================================="
            echo "# Project Structure (simple directory listing)"
            echo "# Excluded: build/, obj/, tools/, misc/, *.md, *.sh, $output_file"
            echo "#===================================="
            echo "# Main directories (excluding build, obj, tools, misc):"
            find . -type d -maxdepth 1 -not -path "*/\.*" -not -path "./build" -not -path "./obj" -not -path "./tools" -not -path "./misc" -not -path "./Books" -not -path "./images" -not -path "./doxygen_docs" | sort
            echo "# ----------------"
            echo "# Files in root (excluding .md, .sh, $output_file):"
            find . -type f -maxdepth 1 -not -path "*/\.*" -not -name '*.md' -not -name '*.sh' -not -name "$output_file" | sort
            echo -e "\n#====================================\n"
        } >> "$output_file"
        tree_output_added=true # Mark as added even if it's the fallback
    fi
fi


total_processed_count=0
processed_shared=0
processed_posix=0
processed_classic_hc=0 # C and H files for classic
processed_classic_r=0  # .r files for classic

# 1. Process Shared Files
process_hc_directory "shared" "$output_file" "$include_headers" "$add_separator" "$separator_line"
processed_shared=$?
total_processed_count=$((total_processed_count + processed_shared))

# 2. Process POSIX Files
if [[ "$platform" == "posix" || "$platform" == "all" ]]; then
    process_hc_directory "posix" "$output_file" "$include_headers" "$add_separator" "$separator_line"
    processed_posix=$?
    total_processed_count=$((total_processed_count + processed_posix))
fi

# 3. Process Classic Mac Files
if [[ "$platform" == "classic" || "$platform" == "all" ]]; then
    # Process .h and .c files
    process_hc_directory "classic_mac" "$output_file" "$include_headers" "$add_separator" "$separator_line"
    processed_classic_hc=$?
    total_processed_count=$((total_processed_count + processed_classic_hc))

    # Process .r files specifically for classic_mac (never strip comments)
    echo "Processing R files in directory: classic_mac"
    while IFS= read -r file; do
        if [ -f "$file" ]; then
            append_file_content "$file" "$output_file" "$include_headers" "$add_separator" "$separator_line"
            ((processed_classic_r++))
        fi
    done < <(find "classic_mac" -maxdepth 1 -name '*.r' 2>/dev/null | sort)
    total_processed_count=$((total_processed_count + processed_classic_r))

fi

# 4. Append Makefiles if requested (never strip comments)
makefiles_found_count=0
if [ "$include_makefile" = true ]; then
    if append_file_content "Makefile" "$output_file" "$include_headers" "$add_separator" "#===================================="; then
        ((makefiles_found_count++))
    fi
    if append_file_content "Makefile.retro68" "$output_file" "$include_headers" "$add_separator" "#===================================="; then
        ((makefiles_found_count++))
    fi
fi

# 5. Append Docker files if requested (never strip comments)
docker_found=false
if [ "$include_docker" = true ]; then
    docker_count=0
    if append_file_content "Dockerfile" "$output_file" "$include_headers" "$add_separator" "#===================================="; then ((docker_count++)); fi
    if append_file_content "docker-compose.yml" "$output_file" "$include_headers" "$add_separator" "#===================================="; then ((docker_count++)); fi
    if append_file_content "docker.sh" "$output_file" "$include_headers" "$add_separator" "#===================================="; then ((docker_count++)); fi
    if [ $docker_count -gt 0 ]; then docker_found=true; fi
fi

echo "------------------------------------"
echo "Code merge complete."
echo "Output written to: $output_file"
if [ "$include_tree" = true ]; then echo "Included tree structure: $tree_output_added"; fi
if [ "$strip_comments" = true ]; then echo "Comment stripping attempted for .c/.h files: Yes"; fi
echo "Total source/resource files processed: $total_processed_count"
echo "  Shared C/H: $processed_shared"
echo "  POSIX C/H: $processed_posix"
echo "  Classic C/H: $processed_classic_hc"
echo "  Classic R: $processed_classic_r"
if [ "$include_makefile" = true ]; then echo "Included Makefile(s): $makefiles_found_count"; fi
if [ "$include_docker" = true ]; then echo "Included Docker files: $docker_found"; fi
