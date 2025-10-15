#!/bin/bash

# CSend Log Filter Script
# Usage: ./filter_logs.sh [options] logfile
#
# Options:
#   -l LEVEL     Show only logs at or above this level (ERROR, WARNING, INFO, DEBUG)
#   -c CATEGORY  Show only logs from this category
#   -p PEER      Show only logs related to a specific peer (username or IP)
#   -t           Show only test-related messages (TEST_R, AUTOMATED TEST markers)
#   -s           Show summary statistics
#   -n NUM       Show only first/last NUM lines (use negative for last)
#   -e           Show only ERROR level logs
#   -w           Show only WARNING and ERROR logs
#   -i           Show only INFO, WARNING and ERROR logs
#   -x           Exclude DEBUG level logs (show INFO and above)
#   -T PATTERN   Show only lines with timestamps matching pattern (e.g., "16:44:2")
#   -C NUM       Show NUM lines of context around matches
#   -h           Show this help message

usage() {
    echo "Usage: $0 [options] logfile"
    echo ""
    echo "Options:"
    echo "  -l LEVEL     Show only logs at or above this level (ERROR, WARNING, INFO, DEBUG)"
    echo "  -c CATEGORY  Show only logs from this category"
    echo "               Categories: GENERAL, NETWORKING, DISCOVERY, PEER_MGMT, UI, PROTOCOL, SYSTEM, MESSAGING"
    echo "  -p PEER      Show only logs related to a specific peer (username or IP)"
    echo "  -t           Show only test-related messages (TEST_R, AUTOMATED TEST markers)"
    echo "  -s           Show summary statistics (message counts, errors, peers, test results)"
    echo "  -n NUM       Show only first/last NUM lines (use negative for last N lines)"
    echo "  -e           Show only ERROR level logs"
    echo "  -w           Show only WARNING and ERROR logs"
    echo "  -i           Show only INFO, WARNING and ERROR logs"
    echo "  -x           Exclude DEBUG level logs (show INFO and above)"
    echo "  -T PATTERN   Show only lines with timestamps matching pattern (e.g., '16:44:2')"
    echo "  -C NUM       Show NUM lines of context around matches (like grep -C)"
    echo "  -h           Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0 -w csend_posix.log                          # Show warnings and errors"
    echo "  $0 -c NETWORKING csend_posix.log               # Show only networking logs"
    echo "  $0 -i -c DISCOVERY csend_posix.log             # Show discovery logs at INFO level or above"
    echo "  $0 -e csend_classic_mac_ot_ppc.log             # Show only errors"
    echo "  $0 -p MacTCP csend_posix.log                   # Show logs related to MacTCP peer"
    echo "  $0 -t csend_posix.log                          # Show only test messages"
    echo "  $0 -s csend_posix.log                          # Show summary statistics"
    echo "  $0 -x -p 10.188.1.213 csend_posix.log          # Non-debug logs for specific IP"
    echo "  $0 -T '16:44:2' csend_posix.log                # Show logs from 16:44:2X"
    echo "  $0 -C 3 -e csend_posix.log                     # Show errors with 3 lines context"
    echo "  $0 -n 100 csend_posix.log                      # Show first 100 lines"
    echo "  $0 -n -50 csend_posix.log                      # Show last 50 lines"
    exit 1
}

LEVEL=""
CATEGORY=""
PEER=""
TEST_ONLY=false
SUMMARY=false
NUM_LINES=""
TIME_PATTERN=""
CONTEXT=""
LOGFILE=""

# Parse command line arguments
while getopts "l:c:p:tsn:xT:C:ewih" opt; do
    case $opt in
        l)
            LEVEL="$OPTARG"
            ;;
        c)
            CATEGORY="$OPTARG"
            ;;
        p)
            PEER="$OPTARG"
            ;;
        t)
            TEST_ONLY=true
            ;;
        s)
            SUMMARY=true
            ;;
        n)
            NUM_LINES="$OPTARG"
            ;;
        x)
            LEVEL="INFO"
            ;;
        T)
            TIME_PATTERN="$OPTARG"
            ;;
        C)
            CONTEXT="$OPTARG"
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

# Function to show summary statistics
show_summary() {
    local file="$1"
    echo "=== Log Summary for $file ==="
    echo ""

    # Basic stats
    echo "Total lines: $(wc -l < "$file")"
    echo "Log session: $(grep -m 1 "Log Session Started" "$file" 2>/dev/null || echo "Unknown")"
    echo ""

    # Level counts
    echo "Log Levels:"
    echo "  ERROR:   $(grep -c "\[ERROR\]" "$file" 2>/dev/null || echo 0)"
    echo "  WARNING: $(grep -c "\[WARNING\]" "$file" 2>/dev/null || echo 0)"
    echo "  INFO:    $(grep -c "\[INFO\]" "$file" 2>/dev/null || echo 0)"
    echo "  DEBUG:   $(grep -c "\[DEBUG\]" "$file" 2>/dev/null || echo 0)"
    echo ""

    # Category counts
    echo "Top Categories:"
    grep -oE "\[(GENERAL|NETWORKING|DISCOVERY|PEER_MGMT|UI|PROTOCOL|SYSTEM|MESSAGING|APP_EVENT)\]" "$file" 2>/dev/null | sort | uniq -c | sort -rn | head -5
    echo ""

    # Peers detected
    echo "Peers Detected:"
    grep -oE "[A-Za-z0-9_-]+@[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+" "$file" 2>/dev/null | sort -u
    echo ""

    # Test results if present
    if grep -q "AUTOMATED TEST" "$file" 2>/dev/null; then
        echo "Test Results:"
        grep "AUTOMATED TEST" "$file" | grep -E "(START|END|Configuration|Summary)"
        echo ""
    fi

    # Connection issues
    local conn_errors=$(grep -c "Connection refused\|Cannot reach\|Failed to" "$file" 2>/dev/null || echo 0)
    if [ "$conn_errors" -gt 0 ]; then
        echo "Connection Issues: $conn_errors occurrences"
        grep "Cannot reach\|Connection refused" "$file" | head -3
        echo ""
    fi

    # Message counts
    echo "Message Activity:"
    echo "  Discoveries sent:  $(grep -c "Discovery broadcast sent" "$file" 2>/dev/null || echo 0)"
    echo "  Messages sent:     $(grep -c "Message sent to peer" "$file" 2>/dev/null || echo 0)"
    echo "  Messages received: $(grep -c "Received message ID" "$file" 2>/dev/null || echo 0)"
}

# If summary requested, show it and exit
if [ "$SUMMARY" = true ]; then
    show_summary "$LOGFILE"
    exit 0
fi

# Build the filtering pipeline
FILTER_CMD="cat \"$LOGFILE\""

# Apply line limit first if negative (tail)
if [ -n "$NUM_LINES" ] && [ "$NUM_LINES" -lt 0 ]; then
    FILTER_CMD="$FILTER_CMD | tail -n ${NUM_LINES#-}"
fi

# Apply timestamp filter
if [ -n "$TIME_PATTERN" ]; then
    FILTER_CMD="$FILTER_CMD | grep '$TIME_PATTERN'"
fi

# Build the grep pattern for level filtering
PATTERN=""
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

# Apply level filter
if [ -n "$PATTERN" ]; then
    if [ -n "$CONTEXT" ]; then
        FILTER_CMD="$FILTER_CMD | grep -E -C $CONTEXT '$PATTERN'"
    else
        FILTER_CMD="$FILTER_CMD | grep -E '$PATTERN'"
    fi
fi

# Apply category filter
if [ -n "$CATEGORY" ]; then
    FILTER_CMD="$FILTER_CMD | grep '\[$CATEGORY\]'"
fi

# Apply peer filter
if [ -n "$PEER" ]; then
    FILTER_CMD="$FILTER_CMD | grep '$PEER'"
fi

# Apply test filter
if [ "$TEST_ONLY" = true ]; then
    FILTER_CMD="$FILTER_CMD | grep -E '(TEST_R|AUTOMATED TEST|Test Round|Perform Test)'"
fi

# Apply positive line limit (head)
if [ -n "$NUM_LINES" ] && [ "$NUM_LINES" -gt 0 ]; then
    FILTER_CMD="$FILTER_CMD | head -n $NUM_LINES"
fi

# Execute the filter pipeline
eval $FILTER_CMD