#!/bin/bash
# Monitor script to launch Magpie.app and kill if memory exceeds 1GB

MAX_MEM_MB=1024  # 1GB limit
APP_NAME="Magpie"

# Kill any existing instances
if pgrep -x "$APP_NAME" > /dev/null; then
    echo "Killing existing $APP_NAME instances..."
    killall "$APP_NAME"
    sleep 1
fi

# Launch the app in background
open Magpie.app &

echo "Launched Magpie.app - monitoring memory usage (max: ${MAX_MEM_MB}MB)"
echo "Press Ctrl-C to stop monitoring and quit the app"

# Monitor loop
while true; do
    sleep 2

    # Get memory usage in MB using ps
    MEM=$(ps -A -o pid,rss,comm | grep "$APP_NAME" | grep -v grep | awk '{print $2}')

    if [ -z "$MEM" ]; then
        echo "App exited normally"
        exit 0
    fi

    # Convert KB to MB
    MEM_MB=$((MEM / 1024))

    echo "[$(date +%T)] Memory usage: ${MEM_MB}MB / ${MAX_MEM_MB}MB"

    if [ $MEM_MB -gt $MAX_MEM_MB ]; then
        echo "⚠️  MEMORY LIMIT EXCEEDED! Killing app..."
        killall "$APP_NAME"
        echo "App killed due to excessive memory usage: ${MEM_MB}MB > ${MAX_MEM_MB}MB"
        exit 1
    fi
done
