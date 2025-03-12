#!/bin/bash

output_file="mbai_files.txt"
> "$output_file"  # Clear the output file before writing

# Root directory
root_dir="/home/josh/Dropbox/BAI/code_baiuv"

# Hardcoded list of mbai files
files=("binary_search.jl" "expfam.jl" "tracking.jl" "baiuv/helpers.jl" "baiuv/peps.jl" "baiuv/samplingrules.jl"  "baiuv/runit.jl" )

for file in "${files[@]}"; do
    full_path="$root_dir/$file"
    if [[ -f "$full_path" ]]; then
        echo "=====================================" >> "$output_file"
        echo "=== START OF FILE: $full_path ===" >> "$output_file"
        echo "=====================================" >> "$output_file"
        cat "$full_path" >> "$output_file"
        echo "=====================================" >> "$output_file"
        echo "=== END OF FILE: $full_path ===" >> "$output_file"
        echo "=====================================" >> "$output_file"
        echo >> "$output_file"
    fi
done
