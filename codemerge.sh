#!/bin/bash

# codemerge.sh - A tool to merge source files for AI assistance and other purposes
# Usage: ./codemerge.sh [options]

# Default values
output_file="combined_code.txt"
include_headers=true
sort_method="alphabetical"
file_pattern="*.c"
target_dir="."
add_separator=true
separator_line="//===================================="

# Display help message
show_help() {
    echo "Usage: $0 [options]"
    echo
    echo "Options:"
    echo "  -o, --output FILE       Specify output file (default: combined_code.txt)"
    echo "  -d, --directory DIR     Specify target directory (default: current directory)"
    echo "  -p, --pattern PATTERN   File pattern to match (default: *.c)"
    echo "  -s, --sort METHOD       Sorting method: alphabetical, dependency, manual (default: alphabetical)"
    echo "  -n, --no-headers        Don't include filename headers"
    echo "  -m, --no-separators     Don't include separator lines between files"
    echo "  -h, --help              Display this help message"
    echo
    echo "Examples:"
    echo "  $0 -o ai_review.txt -p \"*.c\" -s dependency"
    echo "  $0 --directory src --pattern \"*.h\" --sort manual"
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

# Function to get files in dependency order (basic implementation)
get_files_in_dependency_order() {
    local dir="$1"
    local pattern="$2"
    
    # This is a simplified dependency ordering
    # First, find header files that might be included by others
    find "$dir" -name "*.h" | sort
    
    # Then find implementation files
    # We prioritize files that might be core/base functionality
    find "$dir" -name "$pattern" | grep -E '(main|core|base|util|common)' 2>/dev/null || true
    
    # Then the rest of the files
    find "$dir" -name "$pattern" | grep -v -E '(main|core|base|util|common)' 2>/dev/null || true
}

# Function to get files in manual order (interactive)
get_files_in_manual_order() {
    local dir="$1"
    local pattern="$2"
    local files=()
    local ordered_files=()
    
    # Get all matching files
    while IFS= read -r file; do
        files+=("$file")
    done < <(find "$dir" -name "$pattern" | sort)
    
    echo "Manual ordering mode. Select files in the order you want them to appear:"
    
    local i=1
    for file in "${files[@]}"; do
        echo "$i) $file"
        ((i++))
    done
    
    echo "Enter file numbers in desired order (space-separated):"
    read -r file_order
    
    for num in $file_order; do
        if [[ $num -ge 1 && $num -le ${#files[@]} ]]; then
            ordered_files+=("${files[$((num-1))]}")
        fi
    done
    
    # Add any files that weren't manually selected at the end
    for file in "${files[@]}"; do
        if ! echo "${ordered_files[@]}" | grep -q "$file"; then
            ordered_files+=("$file")
        fi
    done
    
    for file in "${ordered_files[@]}"; do
        echo "$file"
    done
}

# Get files based on sorting method
case "$sort_method" in
    alphabetical)
        files=$(find "$target_dir" -name "$file_pattern" | sort)
        ;;
    dependency)
        files=$(get_files_in_dependency_order "$target_dir" "$file_pattern")
        ;;
    manual)
        files=$(get_files_in_manual_order "$target_dir" "$file_pattern")
        ;;
    *)
        echo "Error: Unknown sorting method '$sort_method'"
        exit 1
        ;;
esac

# Process each file
for file in $files; do
    if [ -f "$file" ]; then
        if [ "$include_headers" = true ]; then
            echo -e "\n$separator_line" >> "$output_file"
            echo "// FILE: $file" >> "$output_file"
            echo -e "$separator_line\n" >> "$output_file"
        fi
        
        cat "$file" >> "$output_file"
        
        if [ "$add_separator" = true ]; then
            echo -e "\n\n" >> "$output_file"
        fi
    fi
done

echo "Files combined into $output_file"
echo "Total files processed: $(echo "$files" | wc -w)"