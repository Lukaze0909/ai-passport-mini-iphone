#!/bin/sh
# Minimal COMPANION_HOOK_CMD: append the event to a file and print a summary.
# Replace this with whatever wakes your agent (an HTTP call, a queue, a CLI...).
# The event arrives as one JSON object on stdin; exit 0 means "delivered".
out="${PASSPORT_HOOK_LOG:-./state/hook-events.jsonl}"
cat >> "$out"
echo >> "$out"
echo "queued to $out"
