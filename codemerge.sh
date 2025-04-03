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

# Display help message
show_help() {
    echo "Usage: $0 [options]"
    echo
    echo "Merges source code from shared/, posix/, and classic_mac/ directories."
    echo
    echo "Options:"
    echo "  -o, --output FILE          Specify output file (default: combined_code.txt)"
    echo "  -P, --platform PLATFORM    Specify platform: posix, classic, all (default: all)"
    echo "  -n, --no-headers           Don't include filename headers"
    echo "  -m, --no-separators        Don't include separator lines between files"
    echo "  -M, --include-makefile     Append root Makefile content at the end"
    echo "  -D, --include-docker       Append root Dockerfile, docker-compose.yml and docker.sh content at the end"
    echo "  -h, --help                 Display this help message"
    echo
    echo "Examples:"
    echo "  $0 -o posix_for_ai.txt -P posix -M"
    echo "  $0 --platform classic --no-separators"
    echo "  $0 -o project_snapshot.txt # Defaults to --platform all"
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

# Create or clear the output file
> "$output_file"

# Function to append a file with optional header/separator
# Takes full path as $1, uses it to generate relative path for header
append_file_content() {
    local file_to_append="$1"
    local output_target="$2"
    local use_headers="$3"
    local use_separator="$4"
    local sep_line="$5"
    # Use # for Makefiles/Dockerfiles/Shell, // for C/C++ as a guess
    local header_prefix="//"
    local file_basename=$(basename "$file_to_append")

    if [[ "$file_basename" == *Makefile* || "$file_basename" == *Dockerfile* || "$file_basename" == *docker-compose.yml* || "$file_basename" == *.sh ]]; then
        header_prefix="#"
    fi

    if [ -f "$file_to_append" ]; then
        echo "Appending $file_to_append..."
        if [ "$use_headers" = true ]; then
            echo -e "\n$sep_line" >> "$output_target"
            # Display path relative to script execution dir (which should be project root)
            echo "$header_prefix FILE: ./$file_to_append" >> "$output_target"
            echo -e "$sep_line\n" >> "$output_target"
        elif [ "$use_separator" = true ]; then
             # Add a smaller separator if headers are off but separators are on
             echo -e "\n--- $file_to_append ---\n" >> "$output_target"
        fi

        cat "$file_to_append" >> "$output_target"
        # Add extra newline for spacing, only if separators are enabled
        if [ "$use_separator" = true ]; then
             echo "" >> "$output_target"
        fi
        return 0 # Success
    else
        echo "Warning: File '$file_to_append' not found, skipping."
        return 1 # Failure
    fi
}

# Function to process all .h and .c files in a given directory
process_directory() {
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

    echo "Processing directory: $dir"

    # Process header files first, sorted alphabetically
    while IFS= read -r file; do
        if [ -f "$file" ]; then
            append_file_content "$file" "$output_target" "$use_headers" "$use_separator" "$sep_line"
             ((processed++))
        fi
    done < <(find "$dir" -maxdepth 1 -name '*.h' 2>/dev/null | sort)

    # Process source files, sorted alphabetically
    while IFS= read -r file; do
        if [ -f "$file" ]; then
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

total_processed_count=0
processed_shared=0
processed_posix=0
processed_classic=0

# 1. Process Shared Files (Always included unless platform is invalid)
process_directory "shared" "$output_file" "$include_headers" "$add_separator" "$separator_line"
processed_shared=$?
total_processed_count=$((total_processed_count + processed_shared))

# 2. Process POSIX Files (if selected)
if [[ "$platform" == "posix" || "$platform" == "all" ]]; then
    process_directory "posix" "$output_file" "$include_headers" "$add_separator" "$separator_line"
    processed_posix=$?
    total_processed_count=$((total_processed_count + processed_posix))
fi

# 3. Process Classic Mac Files (if selected)
if [[ "$platform" == "classic" || "$platform" == "all" ]]; then
    process_directory "classic_mac" "$output_file" "$include_headers" "$add_separator" "$separator_line"
    processed_classic=$?
    total_processed_count=$((total_processed_count + processed_classic))
fi

# 4. Append Makefile if requested (looks in root directory)
makefile_found=false
if [ "$include_makefile" = true ]; then
    if append_file_content "Makefile" "$output_file" "$include_headers" "$add_separator" "#===================================="; then
        makefile_found=true
    fi
fi

# 5. Append Docker files if requested (looks in root directory)
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
echo "Total source files processed: $total_processed_count (Shared: $processed_shared, POSIX: $processed_posix, Classic: $processed_classic)"
if [ "$include_makefile" = true ]; then echo "Included Makefile: $makefile_found"; fi
if [ "$include_docker" = true ]; then echo "Included Docker files: $docker_found"; fi
