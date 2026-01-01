#!/bin/bash

# --- Hardcoded Array of Command Pairs ---
# Format: ("command", "description")
# Note: The commands should be enclosed in quotes if they contain spaces.
# The pairs must be separated by spaces.
COMMAND_PAIRS=(
    "git checkout main" "Switching to main branch..."
    "git pull" "Pulling latest changes from remote..."
    "./download_data.sh" "Downloading lexical data..."
    "./convert_lexica.sh"        "Building lexical data..."
    "make clean && make magpie BUILD=release" "Building the magpie executable..."
)

NUM_PAIRS=${#COMMAND_PAIRS[@]}

# Loop through the array, incrementing by 2
for ((i = 0; i < NUM_PAIRS; i += 2)); do
    COMMAND=${COMMAND_PAIRS[i]}
    DESCRIPTION=${COMMAND_PAIRS[i+1]}

    # 1. Print the description
    echo $DESCRIPTION

    # Execute the command, capturing all output (stdout and stderr)
    OUTPUT_OR_ERROR=$(eval "$COMMAND" 2>&1)
    
    # Capture the exit status of the previous command
    EXIT_CODE=$?

    # 2. Check for Failure
    if [ $EXIT_CODE -ne 0 ]; then
        echo -e "\n**🚨 FAILED! (Exit Code: $EXIT_CODE) 🚨**"
        echo "--- Command Output/Error ---"
        # Print the captured output/error
        echo "$OUTPUT_OR_ERROR"
        echo "----------------------------"
        
        # Manually trigger the script exit immediately upon failure
        exit $EXIT_CODE
    fi
done

echo "Finished. You can now invoke MAGPIE with './bin/magpie' from the top-level directory."