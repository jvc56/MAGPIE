#!/bin/bash

# Reset the terminal
tput reset

# Define the branches we want to test
BRANCHES=("autoplay_sims_3" "remove_global_cache")

# Store the starting branch so we can return to it later
STARTING_BRANCH=$(git rev-parse --abbrev-ref HEAD)

for BRANCH in "${BRANCHES[@]}"; do
    echo "------------------------------------------------"
    echo "Switching to branch: $BRANCH"
    echo "------------------------------------------------"

    # Checkout the branch
    git checkout "$BRANCH" || { echo "Failed to checkout $BRANCH"; continue; }

    # Run the build and benchmark
    # We use 'time' on the entire execution of the binary
    make clean && \
    make magpie BUILD=release && \
    time ./bin/magpie autoplay games 50000 -lex CSW24 -seed 12345

    echo -e "\nFinished benchmarking $BRANCH\n"
    
    # Optional: Wait for user input before moving to the next branch
    # read -p "Press enter to continue to the next branch..."
done

# Return to the original branch
git checkout "$STARTING_BRANCH"
echo "Done. Returned to $STARTING_BRANCH."