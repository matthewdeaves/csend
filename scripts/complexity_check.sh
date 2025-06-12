#!/bin/bash

# Script to analyze code complexity using Lizard
# https://github.com/terryyin/lizard

# --- Configuration ---
# Cyclomatic complexity threshold (functions with CCN > this will be warned)
CCN_THRESHOLD=10
# Maximum function length in lines (excluding comments/blanks)
LENGTH_THRESHOLD=60
# Maximum number of parameters for a function
PARAM_THRESHOLD=5
# Directories to analyze
SOURCE_DIRS="posix shared classic_mac"
# File patterns to exclude
EXCLUDE_PATTERNS="classic_mac/DNR.c"

# --- Color codes for output ---
RED='\033[0;31m'
YELLOW='\033[1;33m'
GREEN='\033[0;32m'
NC='\033[0m' # No Color

# --- Functions ---
check_lizard_installed() {
    if ! command -v lizard &> /dev/null; then
        echo -e "${RED}Error: Lizard is not installed.${NC}"
        echo ""
        echo "Installation options:"
        echo "  1. Using pipx (recommended):"
        echo "     pipx install lizard"
        echo ""
        echo "  2. Using pip with --break-system-packages (use with caution):"
        echo "     pip install --break-system-packages lizard"
        echo ""
        echo "  3. In a virtual environment:"
        echo "     python3 -m venv venv"
        echo "     source venv/bin/activate"
        echo "     pip install lizard"
        echo ""
        echo "  4. Using system package manager (if available):"
        echo "     sudo apt install python3-lizard  # Debian/Ubuntu"
        echo ""
        echo "Visit: https://github.com/terryyin/lizard for more info"
        exit 1
    fi
}

print_header() {
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}Code Complexity Analysis with Lizard${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
}

run_basic_analysis() {
    echo -e "${YELLOW}Running basic complexity analysis...${NC}"
    echo "Thresholds:"
    echo "  - Cyclomatic Complexity: $CCN_THRESHOLD"
    echo "  - Function Length: $LENGTH_THRESHOLD lines"
    echo "  - Parameter Count: $PARAM_THRESHOLD"
    echo ""
    
    # Run lizard with our thresholds
    lizard $SOURCE_DIRS \
        -C $CCN_THRESHOLD \
        -L $LENGTH_THRESHOLD \
        -a $PARAM_THRESHOLD \
        -x "$EXCLUDE_PATTERNS" \
        -l c
}

run_warnings_only() {
    echo -e "${YELLOW}Functions exceeding complexity thresholds:${NC}"
    echo ""
    
    # Show only warnings
    lizard $SOURCE_DIRS \
        -C $CCN_THRESHOLD \
        -L $LENGTH_THRESHOLD \
        -a $PARAM_THRESHOLD \
        -x "$EXCLUDE_PATTERNS" \
        -w \
        -l c
}

run_detailed_analysis() {
    echo -e "${YELLOW}Generating detailed report...${NC}"
    
    # Create reports directory if it doesn't exist
    mkdir -p reports
    
    # Generate detailed HTML report
    if command -v lizard &> /dev/null && lizard --help | grep -q "\-\-html"; then
        lizard $SOURCE_DIRS \
            -C $CCN_THRESHOLD \
            -L $LENGTH_THRESHOLD \
            -a $PARAM_THRESHOLD \
            -x "$EXCLUDE_PATTERNS" \
            -l c \
            --html > reports/complexity_report.html
        echo "Detailed HTML report saved to: reports/complexity_report.html"
    fi
    
    # Generate CSV report for further analysis
    lizard $SOURCE_DIRS \
        -C $CCN_THRESHOLD \
        -L $LENGTH_THRESHOLD \
        -a $PARAM_THRESHOLD \
        -x "$EXCLUDE_PATTERNS" \
        -l c \
        --csv > reports/complexity_report.csv
    echo "CSV report saved to: reports/complexity_report.csv"
}

show_summary() {
    echo ""
    echo -e "${GREEN}Analysis complete!${NC}"
    echo ""
    echo "Tips for reducing complexity:"
    echo "  - Extract complex conditions into well-named boolean variables"
    echo "  - Break down large functions into smaller, focused functions"
    echo "  - Replace nested if statements with early returns"
    echo "  - Consider using lookup tables instead of long switch statements"
}

# --- Main script ---
main() {
    check_lizard_installed
    print_header
    
    # Parse command line arguments
    case "${1:-basic}" in
        "basic")
            run_basic_analysis
            ;;
        "warnings")
            run_warnings_only
            ;;
        "detailed")
            run_basic_analysis
            echo ""
            run_detailed_analysis
            ;;
        "help"|"-h"|"--help")
            echo "Usage: $0 [basic|warnings|detailed|help]"
            echo ""
            echo "Options:"
            echo "  basic    - Show full analysis with all functions (default)"
            echo "  warnings - Show only functions exceeding thresholds"
            echo "  detailed - Generate detailed reports in reports/ directory"
            echo "  help     - Show this help message"
            echo ""
            echo "Current thresholds:"
            echo "  - Cyclomatic Complexity: $CCN_THRESHOLD"
            echo "  - Function Length: $LENGTH_THRESHOLD lines"
            echo "  - Parameter Count: $PARAM_THRESHOLD"
            exit 0
            ;;
        *)
            echo -e "${RED}Invalid option: $1${NC}"
            echo "Use '$0 help' for usage information"
            exit 1
            ;;
    esac
    
    show_summary
}

# Run the main function
main "$@"