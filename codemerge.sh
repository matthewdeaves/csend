#!/bin/bash

# codemerge.sh - A tool to merge source files for AI assistance and other purposes
# Usage: ./codemerge.sh [options]

# Default values
output_file="combined_code.txt"
include_headers=true
sort_method="dependency" # Default sort method changed to dependency
file_pattern="*.c"
target_dir="."
add_separator=true
separator_line="//===================================="
include_makefile=false
include_docker=false

# Display help message
show_help() {
    echo "Usage: $0 [options]"
    echo
    echo "Options:"
    echo "  -o, --output FILE       Specify output file (default: combined_code.txt)"
    echo "  -d, --directory DIR     Specify target directory (default: current directory)"
    echo "  -p, --pattern PATTERN   File pattern to match (default: *.c)"
    echo "  -s, --sort METHOD       Sorting method: alphabetical, dependency, manual (default: dependency)"
    echo "  -n, --no-headers        Don't include filename headers"
    echo "  -m, --no-separators     Don't include separator lines between files"
    echo "  -M, --include-makefile  Append Makefile content at the end"
    echo "  -D, --include-docker    Append Dockerfile, docker-compose.yml and docker.sh content at the end"
    echo "  -h, --help              Display this help message"
    echo
    echo "Examples:"
    echo "  $0 -o ai_review.txt -p \"*.py\" -s dependency --include-docker"
    echo "  $0 --directory src --pattern \"*.h\" --sort manual -M"
    echo "  $0 -p \"*.java\" --no-headers"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -o|--output)
            output_file="$2"
            shift 2
            ;;
        -d|--directory)
            target_dir="$2"
            shift 2
            ;;
        -p|--pattern)
            file_pattern="$2"
            shift 2
            ;;
        -s|--sort)
            sort_method="$2"
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

# Check if target directory exists
if [ ! -d "$target_dir" ]; then
    echo "Error: Directory '$target_dir' does not exist."
    exit 1
fi

# Create or clear the output file
> "$output_file"

# Function to append a file with optional header/separator
append_file_content() {
    local file_to_append="$1"
    local output_target="$2"
    local use_headers="$3"
    local use_separator="$4"
    local sep_line="$5"

    if [ -f "$file_to_append" ]; then
        echo "Appending $file_to_append..."
        if [ "$use_headers" = true ]; then
            echo -e "\n$sep_line" >> "$output_target"
            # Use # for Makefiles/Dockerfiles, // for others as a guess
            local header_prefix="//"
            if [[ "$file_to_append" == *Makefile* || "$file_to_append" == *Dockerfile* || "$file_to_append" == *docker-compose.yml* || "$file_to_append" == *docker.sh* ]]; then
                header_prefix="#"
            fi
            echo "$header_prefix FILE: $file_to_append" >> "$output_target"
            echo -e "$sep_line\n" >> "$output_target"
        elif [ "$use_separator" = true ]; then
             # Add a smaller separator if headers are off but separators are on
             echo -e "\n--- $file_to_append ---\n" >> "$output_target"
        fi

        cat "$file_to_append" >> "$output_target"

        if [ "$use_separator" = true ]; then
            echo -e "\n\n" >> "$output_target"
        fi
        return 0 # Success
    else
        echo "Warning: File '$file_to_append' not found in '$target_dir', skipping."
        return 1 # Failure
    fi
}


# Function to get files in dependency order (basic implementation)
get_files_in_dependency_order() {
    local dir="$1"
    local pattern="$2"

    # This is a simplified dependency ordering
    # First, find header files that might be included by others (adjust pattern if needed)
    find "$dir" -maxdepth 1 -name "*.h" 2>/dev/null | sort

    # Then find implementation files
    # We prioritize files that might be core/base functionality
    find "$dir" -maxdepth 1 -name "$pattern" | grep -E '(main|core|base|util|common)' 2>/dev/null || true

    # Then the rest of the files
    find "$dir" -maxdepth 1 -name "$pattern" | grep -v -E '(main|core|base|util|common)' 2>/dev/null || true
}

# Function to get files in manual order (interactive)
get_files_in_manual_order() {
    local dir="$1"
    local pattern="$2"
    local files=()
    local ordered_files=()

    # Get all matching files (only in the target dir, not subdirs)
    while IFS= read -r file; do
        files+=("$file")
    done < <(find "$dir" -maxdepth 1 -name "$pattern" | sort)

    if [ ${#files[@]} -eq 0 ]; then
        echo "No files matching '$pattern' found in '$dir'."
        return
    fi

    echo "Manual ordering mode. Select files in the order you want them to appear:"

    local i=1
    for file in "${files[@]}"; do
        echo "$i) $(basename "$file")" # Show only basename for clarity
        ((i++))
    done

    echo "Enter file numbers in desired order (space-separated), or press Enter to skip:"
    read -r file_order

    if [[ -z "$file_order" ]]; then
         echo "Skipping manual order, using alphabetical."
         for file in "${files[@]}"; do echo "$file"; done
         return
    fi

    local selected_indices=()
    for num in $file_order; do
        if [[ "$num" =~ ^[0-9]+$ ]] && [ "$num" -ge 1 ] && [ "$num" -le ${#files[@]} ]; then
            local index=$((num-1))
            # Avoid adding duplicates if user enters same number twice
            if [[ ! " ${selected_indices[@]} " =~ " ${index} " ]]; then
                 ordered_files+=("${files[$index]}")
                 selected_indices+=("$index")
            fi
        else
            echo "Warning: Invalid input '$num', skipping."
        fi
    done

    # Add any files that weren't manually selected at the end (alphabetically)
    local remaining_files=()
    local i=0
    for file in "${files[@]}"; do
        is_selected=false
        for selected_index in "${selected_indices[@]}"; do
            if [[ $i -eq $selected_index ]]; then
                is_selected=true
                break
            fi
        done
        if ! $is_selected; then
            remaining_files+=("$file")
        fi
        ((i++))
    done

    # Append remaining files to the ordered list
    ordered_files+=("${remaining_files[@]}")


    for file in "${ordered_files[@]}"; do
        echo "$file"
    done
}

# --- Main Processing ---

echo "Starting code merge..."
echo "Output file: $output_file"
echo "Target directory: $target_dir"
echo "File pattern: $file_pattern"
echo "Sort method: $sort_method"

# Get primary code files based on sorting method
files_to_process=()
processed_count=0

case "$sort_method" in
    alphabetical)
        while IFS= read -r file; do files_to_process+=("$file"); done < <(find "$target_dir" -maxdepth 1 -name "$file_pattern" | sort)
        ;;
    dependency)
        while IFS= read -r file; do files_to_process+=("$file"); done < <(get_files_in_dependency_order "$target_dir" "$file_pattern")
        ;;
    manual)
        while IFS= read -r file; do files_to_process+=("$file"); done < <(get_files_in_manual_order "$target_dir" "$file_pattern")
        ;;
    *)
        echo "Error: Unknown sorting method '$sort_method'"
        exit 1
        ;;
esac

# Process each primary code file
if [ ${#files_to_process[@]} -gt 0 ]; then
    echo "Processing ${#files_to_process[@]} files matching '$file_pattern'..."
    for file in "${files_to_process[@]}"; do
        if [ -f "$file" ]; then
            append_file_content "$file" "$output_file" "$include_headers" "$add_separator" "$separator_line"
            ((processed_count++))
        fi
    done
else
    echo "No files found matching pattern '$file_pattern' in '$target_dir'."
fi


# Append Makefile if requested
if [ "$include_makefile" = true ]; then
    makefile_path="$target_dir/Makefile"
    append_file_content "$makefile_path" "$output_file" "$include_headers" "$add_separator" "#====================================" # Use # separator for Makefile
fi

# Append Docker files if requested
if [ "$include_docker" = true ]; then
    dockerfile_path="$target_dir/Dockerfile"
    composefile_path="$target_dir/docker-compose.yml"
    containerfile_path="$target_dir/docker.sh"
    append_file_content "$dockerfile_path" "$output_file" "$include_headers" "$add_separator" "#====================================" # Use # separator for Dockerfile
    append_file_content "$composefile_path" "$output_file" "$include_headers" "$add_separator" "#====================================" # Use # separator for docker-compose
    append_file_content "$containerfile_path" "$output_file" "$include_headers" "$add_separator" "#====================================" # Use # separator for container.sh
fi

echo "------------------------------------"
echo "Code merge complete."
echo "Output written to: $output_file"
echo "Total primary files processed: $processed_count"
if [ "$include_makefile" = true ]; then echo "Included Makefile (if found)."; fi
if [ "$include_docker" = true ]; then echo "Included Dockerfile and docker-compose.yml (if found)."; fi