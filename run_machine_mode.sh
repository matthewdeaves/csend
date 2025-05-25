#!/bin/bash
# Simple helper to run CSend in machine mode

USERNAME="${1:-claude}"
echo "Starting CSend in machine mode as '$USERNAME'"
echo "Commands: /list, /send <num> <msg>, /broadcast <msg>, /quit"
echo "----------------------------------------"

./build/posix/csend_posix --machine-mode "$USERNAME"