#!/bin/bash

output_file="bai_files.txt"
> "$output_file"  # Clear the output file before writing

for file in bai*; do
    if [[ -f "$file" && "$file" != "$output_file" ]]; then
        echo "=====================================" >> "$output_file"
        echo "=== START OF FILE: $file ===" >> "$output_file"
        echo "=====================================" >> "$output_file"
        cat "$file" >> "$output_file"
        echo "=====================================" >> "$output_file"
        echo "=== END OF FILE: $file ===" >> "$output_file"
        echo "=====================================" >> "$output_file"
        echo >> "$output_file"
    fi
done

