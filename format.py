import difflib
import os
import re
import sys
import argparse
import subprocess
import tempfile
from typing import List, Tuple # Import List and Tuple for Python 3.6+ compatibility

# Compile regex patterns once for efficiency
# Matches lines starting with optional whitespace, followed by #include,
# then whitespace, then a quoted or angled path, and optional trailing whitespace.
INCLUDE_PATTERN = re.compile(r'^\s*#include\s+["<].*[">]\s*$')
# Matches lines that are entirely whitespace or empty.
EMPTY_LINE_PATTERN = re.compile(r'^\s*$', re.MULTILINE)
FORMAT_EXEC = "clang-format-20"

# Define the directories to be ignored (can be multiple)
IGNORED_DIRECTORIES = [
    os.path.normpath('src/compat'),
    # Add other directories to ignore here, e.g., os.path.normpath('src/old_code')
]

def format_c_or_h_file(filepath: str, write_changes: bool) -> Tuple[bool, bool]:
    """
    Formats a single .c/.h file by organizing its include statements according to specified rules.
    It can either write the changes back to the file or just report if changes would occur.

    Operations performed:
    1. Ensures include statements are contiguous (only includes or empty lines between first and last include).
       Prints an error and skips if not contiguous.
    2. Removes all empty lines within the include statement block.
    3. If the file is a .c file and includes its corresponding .h file, moves that include
       to the top of the include block and appends an empty line after it.
    4. Applies formatter to the result of the above steps.
    5. If 'write_changes' is True, writes the final formatted content back to the file in place.
       Otherwise, it only compares the old and new content (after format) and reports differences.

    Args:
        filepath: The absolute or relative path to the .c or .h file to format.
        write_changes: A boolean indicating whether to write changes back to the file (True)
                       or just compare and report differences (False).

    Returns:
        A tuple (is_error, has_diff):
        - is_error: True if a non-contiguous include block was found, or if the file
                    could not be read.
        - has_diff: True if the file content would change after both formatting steps.
    """
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            lines = f.readlines()
        original_content_str = "".join(lines) # Store original content for comparison
    except IOError as e:
        print(f"Error: Could not read file '{filepath}': {e}")
        return (True, False) # Indicate an error, no diff since file couldn't be read

    first_include_idx = -1
    last_include_idx = -1

    # Step 1: Find the first include statement in the file
    for i, line in enumerate(lines):
        if INCLUDE_PATTERN.match(line):
            first_include_idx = i
            break

    # If no include statements are found, there's nothing for my script to format
    # The content for the format command will be the original content.
    if first_include_idx == -1:
        my_formatted_content = lines
    else:
        # Step 2: Find the last include statement, searching backwards from the end of the file
        for i in range(len(lines) - 1, first_include_idx - 1, -1):
            if INCLUDE_PATTERN.match(lines[i]):
                last_include_idx = i
                break

        # Step 3: Check for contiguity of the include block.
        for i in range(first_include_idx, last_include_idx + 1):
            line = lines[i]
            if not INCLUDE_PATTERN.match(line) and not EMPTY_LINE_PATTERN.match(line):
                print(f"Error: Non-include or non-empty line found within the include block "
                      f"in '{filepath}' at line {i + 1}. Skipping file.")
                return (True, False) # Indicate an error for non-contiguous includes

        # If we reach here, includes are contiguous. Proceed with formatting.

        # Step 4: Separate the file content into three logical parts
        pre_include_content = lines[:first_include_idx]
        include_block_raw = lines[first_include_idx : last_include_idx + 1]
        post_include_content = lines[last_include_idx + 1:]

        # Step 5: Process the raw include block.
        cleaned_includes: List[str] = []
        for line in include_block_raw:
            if INCLUDE_PATTERN.match(line):
                cleaned_includes.append(line.strip())

        self_header_include_content = None
        # Step 6: Apply the self-header rule only for .c files.
        if filepath.endswith('.c'):
            basename = os.path.basename(filepath)
            filename_without_ext = os.path.splitext(basename)[0]
            expected_header_include_double_quotes = f'#include "{filename_without_ext}.h"'
            expected_header_include_angle_brackets = f'#include <{filename_without_ext}.h>'

            for i, inc_line_content in enumerate(cleaned_includes):
                if inc_line_content == expected_header_include_double_quotes or \
                   inc_line_content == expected_header_include_angle_brackets:
                    self_header_include_content = cleaned_includes.pop(i)
                    break

        # Step 7: Reconstruct the final include block based on the rules.
        final_include_block_lines: List[str] = []
        if self_header_include_content:
            final_include_block_lines.append(self_header_include_content + '\n')
            final_include_block_lines.append('\n')
        for inc_content in cleaned_includes:
            final_include_block_lines.append(inc_content + '\n')

        # Combine all parts to form the content after my script's formatting
        my_formatted_content = pre_include_content + final_include_block_lines + post_include_content

    # Now, apply the formatter to the content generated by my script (or original if no includes)
    final_formatted_content_str = "".join(my_formatted_content) # Start with my script's output

    # Create a temporary file to apply the formatter
    # Use delete=False to ensure the file exists after creation for subprocess, then delete manually
    temp_file = None
    try:
        # Use the original file's extension for the temporary file to help the formatter
        with tempfile.NamedTemporaryFile(mode='w+', delete=False, encoding='utf-8', suffix=os.path.splitext(filepath)[1]) as temp_f:
            temp_file = temp_f.name
            temp_f.write(final_formatted_content_str)
            temp_f.flush() # Ensure content is written to disk before the formatter reads it

        # Apply the formatter to the temporary file
        try:
            # Use check=True to raise CalledProcessError on non-zero exit codes
            # capture_output=True to prevent the formatter from printing to stdout/stderr directly
            subprocess.run([FORMAT_EXEC, '-i', temp_file], check=True, capture_output=True)
        except FileNotFoundError:
            print(f"Warning: '{FORMAT_EXEC}' not found. Skipping external formatting for '{filepath}'.")
        except subprocess.CalledProcessError as e:
            print(f"Error applying '{FORMAT_EXEC}' to temporary file for '{filepath}': {e.stderr.decode().strip()}")
            # If the formatter fails, we still proceed with my script's output for comparison
            # or writing, but log the error. The content of temp_file might be unchanged.

        # Read the content back from the temporary file after the formatter has potentially modified it
        with open(temp_file, 'r', encoding='utf-8') as temp_f:
            final_formatted_content_str = temp_f.read()

    finally:
        if temp_file and os.path.exists(temp_file):
            os.remove(temp_file) # Clean up the temporary file

    # Determine if there are any differences between original and final formatted content
    has_diff = (original_content_str != final_formatted_content_str)

    # Step 9: Handle writing or reporting based on 'write_changes' flag.
    if write_changes:
        if has_diff:
            try:
                with open(filepath, 'w', encoding='utf-8') as f:
                    f.write(final_formatted_content_str)
                print(f"Wrote changes to '{filepath}'.")
            except IOError as e:
                print(f"Error: Could not write to file '{filepath}': {e}")
                # If write fails, it's an error from this function's perspective
                return (False, True) # Still report diff, but indicate write failed
        else:
            print(f"No changes needed for '{filepath}'.")
    else: # Default mode: compare only
        if has_diff:
            # Print header without an extra trailing blank line to avoid double-spacing
            print(f"Differences detected in '{filepath}':")
            original_lines = original_content_str.splitlines(keepends=True)
            final_lines = final_formatted_content_str.splitlines(keepends=True)
            diff_lines = difflib.unified_diff(
                original_lines,
                final_lines,
                fromfile=f"{filepath} (original)",
                tofile=f"{filepath} (formatted)",
                lineterm=""
            )
            for dl in diff_lines:
                # Ensure each diff line is printed with exactly one newline (strip any existing)
                sys.stdout.write(dl.rstrip("\n") + "\n")
        else:
            print(f"No differences detected in '{filepath}'.")

    return (False, has_diff) # No contiguity error, return diff status

def main():
    """
    Main function to parse command-line arguments and initiate the file formatting process.
    It handles discovering files based on the provided argument (single file or all in target dirs).
    It also manages the overall exit status based on formatting results.
    """
    parser = argparse.ArgumentParser(
        description="A C/C++ code formatter script to organize include statements."
    )
    parser.add_argument(
        'filename',
        nargs='?', # Makes the argument optional
        help="Optional: Basename (e.g., 'my_file.c' or 'my_header.h') of a single "
             "file to format. If not provided, all .c and .h files in 'src/', 'cmd/', "
             "and 'test/' directories will be processed recursively."
    )
    parser.add_argument(
        '--write',
        action='store_true',
        help='Write the changes back to the file in place. By default, the script '
             'only checks for differences and reports them without writing.'
    )
    args = parser.parse_args()

    # Define the target directories for recursive processing
    target_directories: List[str] = ['src', 'cmd', 'test']
    files_to_process: List[str] = []

    if args.filename:
        # If a specific filename is provided, search for it in the target directories.
        found_specific_file = False
        for target_dir in target_directories:
            if not os.path.isdir(target_dir):
                continue # Skip if directory doesn't exist

            for root, _, files in os.walk(target_dir):
                for file in files:
                    full_filepath = os.path.join(root, file)
                    # Skip files in any of the ignored directories
                    if any(os.path.normpath(full_filepath).startswith(ignored_dir) for ignored_dir in IGNORED_DIRECTORIES):
                        continue

                    if file == args.filename and (file.endswith('.c') or file.endswith('.h')):
                        files_to_process.append(full_filepath)
                        found_specific_file = True
                        break
                if found_specific_file:
                    break
            if found_specific_file:
                break

        if not files_to_process:
            ignored_dirs_str = ", ".join(f"'{d}'" for d in IGNORED_DIRECTORIES)
            print(f"Error: Specified file '{args.filename}' not found in any of "
                  f"{', '.join(target_directories)} (excluding {ignored_dirs_str}) or is not a .c/.h file.")
            sys.exit(1)
    else:
        # If no specific filename is provided, process all .c and .h files
        # in the defined target directories recursively.
        for target_dir in target_directories:
            if not os.path.isdir(target_dir):
                print(f"Warning: Directory '{target_dir}' not found. Skipping it.")
                continue

            for root, _, files in os.walk(target_dir):
                # Skip the ignored directories and their subdirectories
                if any(os.path.normpath(root).startswith(ignored_dir) for ignored_dir in IGNORED_DIRECTORIES):
                    continue

                for file in files:
                    if file.endswith('.c') or file.endswith('.h'):
                        files_to_process.append(os.path.join(root, file))

    if not files_to_process:
        ignored_dirs_str = ", ".join(f"'{d}'" for d in IGNORED_DIRECTORIES)
        print(f"No .c or .h files found to format in the specified directories (excluding {ignored_dirs_str}).")
        sys.exit(0) # Exit normally if no files were found to process

    overall_non_contiguous_error = False
    overall_diff_found = False

    # Iterate through the identified files and apply the formatting (or check)
    for filepath in files_to_process:
        is_error, has_diff = format_c_or_h_file(filepath, args.write)
        if is_error:
            overall_non_contiguous_error = True
        if has_diff:
            overall_diff_found = True

    # Determine the final exit code based on the results
    if overall_non_contiguous_error:
        print("\nExiting with error: Some files had non-contiguous include blocks.")
        sys.exit(1)
    elif overall_diff_found and not args.write:
        print("\nExiting with error: Differences detected in one or more files (run with --write to apply changes).")
        sys.exit(1)
    else:
        print("\nFormatting check completed successfully." if not args.write else "\nFormatting applied successfully.")
        sys.exit(0)

if __name__ == "__main__":
    main()

