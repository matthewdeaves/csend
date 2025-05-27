#!/bin/bash

# CSend Log Filter Script
# Usage: ./filter_logs.sh [options] logfile
#
# Options:
#   -l LEVEL     Show only logs at or above this level (ERROR, WARNING, INFO, DEBUG)
#   -c CATEGORY  Show only logs from this category
#   -e           Show only ERROR level logs
#   -w           Show only WARNING and ERROR logs
#   -i           Show only INFO, WARNING and ERROR logs
#   -h           Show this help message

usage() {
    echo "Usage: $0 [options] logfile"
    echo ""
    echo "Options:"
    echo "  -l LEVEL     Show only logs at or above this level (ERROR, WARNING, INFO, DEBUG)"
    echo "  -c CATEGORY  Show only logs from this category"
    echo "               Categories: GENERAL, NETWORKING, DISCOVERY, PEER_MGMT, UI, PROTOCOL, SYSTEM, MESSAGING"
    echo "  -e           Show only ERROR level logs"
    echo "  -w           Show only WARNING and ERROR logs"
    echo "  -i           Show only INFO, WARNING and ERROR logs"
    echo "  -h           Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 -l WARNING app_posix.log            # Show warnings and errors"
    echo "  $0 -c NETWORKING app_posix.log         # Show only networking logs"
    echo "  $0 -l INFO -c DISCOVERY app_posix.log  # Show discovery logs at INFO level or above"
    echo "  $0 -e app_classic_mac.log              # Show only errors"
    exit 1
}

LEVEL=""
CATEGORY=""
LOGFILE=""

# Parse command line arguments
while getopts "l:c:ewih" opt; do
    case $opt in
        l)
            LEVEL="$OPTARG"
            ;;
        c)
            CATEGORY="$OPTARG"
            ;;
        e)
            LEVEL="ERROR"
            ;;
        w)
            LEVEL="WARNING"
            ;;
        i)
            LEVEL="INFO"
            ;;
        h)
            usage
            ;;
        \?)
            echo "Invalid option: -$OPTARG" >&2
            usage
            ;;
    esac
done

# Get the log file name
shift $((OPTIND-1))
LOGFILE="$1"

if [ -z "$LOGFILE" ]; then
    echo "Error: No log file specified" >&2
    usage
fi

if [ ! -f "$LOGFILE" ]; then
    echo "Error: Log file '$LOGFILE' not found" >&2
    exit 1
fi

# Build the grep pattern
PATTERN=""

# Add level filtering
if [ -n "$LEVEL" ]; then
    case $LEVEL in
        ERROR)
            PATTERN="\[ERROR\]"
            ;;
        WARNING)
            PATTERN="\[(ERROR|WARNING)\]"
            ;;
        INFO)
            PATTERN="\[(ERROR|WARNING|INFO)\]"
            ;;
        DEBUG)
            PATTERN="\[(ERROR|WARNING|INFO|DEBUG)\]"
            ;;
        *)
            echo "Error: Invalid level '$LEVEL'" >&2
            echo "Valid levels: ERROR, WARNING, INFO, DEBUG" >&2
            exit 1
            ;;
    esac
fi

# Add category filtering
if [ -n "$CATEGORY" ]; then
    if [ -n "$PATTERN" ]; then
        # Combine with level pattern
        grep -E "$PATTERN" "$LOGFILE" | grep "\[$CATEGORY\]"
    else
        # Category only
        grep "\[$CATEGORY\]" "$LOGFILE"
    fi
else
    if [ -n "$PATTERN" ]; then
        # Level only
        grep -E "$PATTERN" "$LOGFILE"
    else
        # No filters, show all
        cat "$LOGFILE"
    fi
fi