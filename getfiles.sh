#!/bin/bash

# Define the list of files to concatenate
files=(
    "classic_mac/network_abstraction.h"
    "classic_mac/network_abstraction.c"
    "classic_mac/mactcp_impl.h"
    "classic_mac/mactcp_impl.c"
    "classic_mac/mactcp_messaging.h"
    "classic_mac/mactcp_messaging.c"
    "classic_mac/mactcp_discovery.h"
    "classic_mac/mactcp_discovery.c"
    "classic_mac/mactcp_network.h"
    "shared/common_defs.h"
)

# Define the output file
output_file="concatenated_output.txt"

# Clear the output file or create it if it doesn't exist
> "$output_file"

# Loop through the files and append their content with a delineator
for file_path in "${files[@]}"; do
    if [ -f "$file_path" ]; then
        # Append the file path as a delineator
        echo "--- START OF FILE: $file_path ---" >> "$output_file"
        echo "" >> "$output_file" # Add an empty line for readability

        # Append the content of the file
        cat "$file_path" >> "$output_file"

        # Add a delineator for the end of the file content
        echo "" >> "$output_file" # Add an empty line for readability
        echo "--- END OF FILE: $file_path ---" >> "$output_file"
        echo "" >> "$output_file" # Add an extra empty line between files
        echo "" >> "$output_file" # Add another extra empty line for more separation
    else
        echo "Warning: File not found - $file_path" >> "$output_file"
        echo "Warning: File not found - $file_path" >&2 # Also print warning to stderr
    fi
done

echo "Concatenation complete. Output written to $output_file"