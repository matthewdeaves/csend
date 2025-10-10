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
SOURCE_DIRS="posix shared classic_mac_mactcp classic_mac_ot"
# File patterns to exclude
EXCLUDE_PATTERNS="*/DNR.c"
# Reports base directory
REPORTS_BASE_DIR="reports/complexity"
# Timestamp for this run
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
# Reports directory for this run
REPORTS_DIR="${REPORTS_BASE_DIR}/${TIMESTAMP}"

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
    mkdir -p "$REPORTS_DIR"

    # Count existing reports
    if [ -d "$REPORTS_BASE_DIR" ]; then
        EXISTING_COUNT=$(find "$REPORTS_BASE_DIR" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l)
        if [ "$EXISTING_COUNT" -gt 1 ]; then
            echo "Found $((EXISTING_COUNT - 1)) existing complexity report(s), creating new report for ${TIMESTAMP}"
        fi
    fi
    echo "Report directory: ${REPORTS_DIR}"
    echo ""

    # Generate detailed HTML report
    if command -v lizard &> /dev/null && lizard --help | grep -q "\-\-html"; then
        lizard $SOURCE_DIRS \
            -C $CCN_THRESHOLD \
            -L $LENGTH_THRESHOLD \
            -a $PARAM_THRESHOLD \
            -x "$EXCLUDE_PATTERNS" \
            -l c \
            --html > "$REPORTS_DIR/complexity_report.html"
        echo "Detailed HTML report saved to: $REPORTS_DIR/complexity_report.html"
    fi

    # Generate CSV report for further analysis
    lizard $SOURCE_DIRS \
        -C $CCN_THRESHOLD \
        -L $LENGTH_THRESHOLD \
        -a $PARAM_THRESHOLD \
        -x "$EXCLUDE_PATTERNS" \
        -l c \
        --csv > "$REPORTS_DIR/complexity_report.csv"
    echo "CSV report saved to: $REPORTS_DIR/complexity_report.csv"

    # Generate text report with warnings
    lizard $SOURCE_DIRS \
        -C $CCN_THRESHOLD \
        -L $LENGTH_THRESHOLD \
        -a $PARAM_THRESHOLD \
        -x "$EXCLUDE_PATTERNS" \
        -w \
        -l c > "$REPORTS_DIR/complexity_warnings.txt" 2>&1

    # Generate LLM summary
    generate_llm_summary
}

generate_llm_summary() {
    LLM_SUMMARY="$REPORTS_DIR/llm_summary.txt"

    {
        echo "CODE COMPLEXITY ANALYSIS - LLM SUMMARY"
        echo "======================================"
        echo "Generated: $(date)"
        echo "Thresholds:"
        echo "  - Cyclomatic Complexity (CCN): $CCN_THRESHOLD"
        echo "  - Function Length: $LENGTH_THRESHOLD lines"
        echo "  - Parameter Count: $PARAM_THRESHOLD"
        echo ""

        # Parse warnings from the warnings file
        if [ -f "$REPORTS_DIR/complexity_warnings.txt" ]; then
            WARNING_COUNT=$(grep -c "!!!!!" "$REPORTS_DIR/complexity_warnings.txt" 2>/dev/null || echo 0)

            echo "## EXECUTIVE SUMMARY"
            echo ""
            if [ "$WARNING_COUNT" -gt 0 ]; then
                echo "⚠ Found $WARNING_COUNT function(s) exceeding complexity thresholds"
            else
                echo "✓ All functions are within acceptable complexity thresholds"
            fi
            echo ""

            if [ "$WARNING_COUNT" -gt 0 ]; then
                echo "## FUNCTIONS EXCEEDING THRESHOLDS"
                echo ""
                grep "!!!!!" "$REPORTS_DIR/complexity_warnings.txt" 2>/dev/null | while IFS= read -r line; do
                    echo "  $line"
                done
                echo ""

                echo "## ANALYSIS BY METRIC"
                echo ""

                # Extract complexity issues
                CCN_VIOLATIONS=$(grep "!!!!!" "$REPORTS_DIR/complexity_warnings.txt" 2>/dev/null | awk '{print $1, $2, $6}' | sort -k1 -rn | head -10)
                if [ -n "$CCN_VIOLATIONS" ]; then
                    echo "Highest Cyclomatic Complexity (CCN > $CCN_THRESHOLD):"
                    echo "$CCN_VIOLATIONS" | while read ccn nloc func; do
                        echo "  CCN=$ccn, Lines=$nloc: $func"
                    done
                    echo ""
                fi

                # Extract length issues
                LENGTH_VIOLATIONS=$(grep "!!!!!" "$REPORTS_DIR/complexity_warnings.txt" 2>/dev/null | awk '{if ($2 > '"$LENGTH_THRESHOLD"') print $2, $1, $6}' | sort -k1 -rn | head -10)
                if [ -n "$LENGTH_VIOLATIONS" ]; then
                    echo "Longest Functions (> $LENGTH_THRESHOLD lines):"
                    echo "$LENGTH_VIOLATIONS" | while read lines ccn func; do
                        echo "  $lines lines, CCN=$ccn: $func"
                    done
                    echo ""
                fi

                # Extract parameter issues
                PARAM_VIOLATIONS=$(grep "!!!!!" "$REPORTS_DIR/complexity_warnings.txt" 2>/dev/null | awk '{if ($3 > '"$PARAM_THRESHOLD"') print $3, $6}' | sort -k1 -rn | head -10)
                if [ -n "$PARAM_VIOLATIONS" ]; then
                    echo "Too Many Parameters (> $PARAM_THRESHOLD):"
                    echo "$PARAM_VIOLATIONS" | while read params func; do
                        echo "  $params parameters: $func"
                    done
                    echo ""
                fi

                echo "## RECOMMENDATIONS"
                echo ""
                echo "1. HIGH PRIORITY - Functions with CCN > $((CCN_THRESHOLD * 2)):"
                grep "!!!!!" "$REPORTS_DIR/complexity_warnings.txt" 2>/dev/null | awk '{if ($1 > '"$((CCN_THRESHOLD * 2))"') print "     "$6" (CCN="$1")"}' | head -5
                echo ""
                echo "2. Refactoring strategies:"
                echo "   - Extract complex conditions into well-named boolean variables"
                echo "   - Break down large functions into smaller, focused functions"
                echo "   - Replace nested if statements with early returns"
                echo "   - Use lookup tables instead of long switch statements"
                echo "   - Consider state machines for complex state handling"
                echo ""
                echo "3. Review functions with > $PARAM_THRESHOLD parameters:"
                echo "   - Group related parameters into structs"
                echo "   - Use builder patterns for complex initialization"
                echo ""
            fi

            # Overall statistics
            if [ -f "$REPORTS_DIR/complexity_report.csv" ]; then
                echo "## OVERALL STATISTICS"
                echo ""
                TOTAL_FUNCTIONS=$(tail -n +2 "$REPORTS_DIR/complexity_report.csv" 2>/dev/null | wc -l)
                AVG_CCN=$(tail -n +2 "$REPORTS_DIR/complexity_report.csv" 2>/dev/null | awk -F',' '{sum+=$2; count++} END {if(count>0) printf "%.1f", sum/count; else print "0"}')
                AVG_LENGTH=$(tail -n +2 "$REPORTS_DIR/complexity_report.csv" 2>/dev/null | awk -F',' '{sum+=$1; count++} END {if(count>0) printf "%.1f", sum/count; else print "0"}')

                echo "Total functions analyzed: $TOTAL_FUNCTIONS"
                echo "Average CCN: $AVG_CCN"
                echo "Average function length: $AVG_LENGTH lines"
                echo "Functions exceeding thresholds: $WARNING_COUNT ($((WARNING_COUNT * 100 / TOTAL_FUNCTIONS))%)"
            fi
        else
            echo "## RESULT"
            echo ""
            echo "No warnings file generated. Run 'detailed' mode to generate reports."
        fi

        echo ""
        echo "## REPORT FILES"
        echo ""
        echo "  - complexity_warnings.txt : Functions exceeding thresholds"
        echo "  - complexity_report.html  : Interactive HTML report"
        echo "  - complexity_report.csv   : CSV data for analysis"
        echo "  - llm_summary.txt         : This summary"

    } > "$LLM_SUMMARY"

    echo ""
    echo "Reports saved to: $REPORTS_DIR/"
    echo "  - complexity_report.html   : Interactive visualization"
    echo "  - complexity_report.csv    : CSV data for analysis"
    echo "  - complexity_warnings.txt  : Functions exceeding thresholds"
    echo "  - llm_summary.txt          : LLM-optimized summary"
    echo ""
    echo "💡 LLM Summary: $REPORTS_DIR/llm_summary.txt"
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