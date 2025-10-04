#!/bin/bash

# Script to detect dead code, unused functions, and unreachable code using GCC
# Uses various GCC flags and compilation techniques

# --- Configuration ---
# Default: analyze all platforms
PLATFORM="all"
# Build directory for analysis
ANALYSIS_BUILD_DIR="build/deadcode_analysis"
# Reports directory
REPORTS_DIR="reports"

# --- Color codes for output ---
RED='\033[0;31m'
YELLOW='\033[1;33m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# --- Retro68 Detection ---
RETRO68_M68K_GCC=""
RETRO68_PPC_GCC=""
RETRO68_PATH=""

detect_retro68() {
    # Check for m68k compiler
    if command -v m68k-apple-macos-gcc &> /dev/null; then
        RETRO68_M68K_GCC="m68k-apple-macos-gcc"
        # Extract Retro68 path from compiler location
        RETRO68_PATH=$(dirname $(dirname $(which m68k-apple-macos-gcc)))
        echo -e "${GREEN}Found Retro68 m68k compiler${NC}"
    fi
    
    # Check for PowerPC compiler
    if command -v powerpc-apple-macos-gcc &> /dev/null; then
        RETRO68_PPC_GCC="powerpc-apple-macos-gcc"
        if [ -z "$RETRO68_PATH" ]; then
            RETRO68_PATH=$(dirname $(dirname $(which powerpc-apple-macos-gcc)))
        fi
        echo -e "${GREEN}Found Retro68 PowerPC compiler${NC}"
    fi
    
    if [ -z "$RETRO68_M68K_GCC" ] && [ -z "$RETRO68_PPC_GCC" ]; then
        echo -e "${YELLOW}Retro68 not found - Classic Mac analysis will be limited${NC}"
    fi
}

# Get appropriate compiler for a file
get_compiler_for_file() {
    local file="$1"

    if [[ "$file" == classic_mac/* ]] || [[ "$file" == classic_mac_ot/* ]]; then
        # Prefer m68k compiler for classic mac
        if [ -n "$RETRO68_M68K_GCC" ]; then
            echo "$RETRO68_M68K_GCC"
        elif [ -n "$RETRO68_PPC_GCC" ]; then
            echo "$RETRO68_PPC_GCC"
        else
            echo "gcc"  # Fallback - will cause errors
        fi
    else
        echo "gcc"
    fi
}

# Get include flags for a file
get_include_flags_for_file() {
    local file="$1"

    if [[ "$file" == classic_mac/* ]] && [ -n "$RETRO68_PATH" ]; then
        # Use Retro68 includes for Classic Mac MacTCP files
        echo "-I\"$RETRO68_PATH/m68k-apple-macos/include\" -I\"$RETRO68_PATH/universal/CIncludes\" -Iclassic_mac -Ishared -D__MACOS__"
    elif [[ "$file" == classic_mac_ot/* ]] && [ -n "$RETRO68_PATH" ]; then
        # Use Retro68 includes for Classic Mac OpenTransport files
        echo "-I\"$RETRO68_PATH/m68k-apple-macos/include\" -I\"$RETRO68_PATH/universal/CIncludes\" -Iclassic_mac_ot -Ishared -D__MACOS__"
    else
        # Standard includes for POSIX
        echo "-Iposix -Ishared -Iclassic_mac -Iclassic_mac_ot"
    fi
}

# --- Functions ---
set_platform_dirs() {
    case "$PLATFORM" in
        "posix")
            SOURCE_DIRS="posix shared"
            echo "Analyzing POSIX platform only"
            ;;
        "classic")
            SOURCE_DIRS="classic_mac classic_mac_ot shared"
            echo "Analyzing Classic Mac platforms (MacTCP + OpenTransport)"
            ;;
        "all")
            SOURCE_DIRS="posix shared classic_mac classic_mac_ot"
            echo "Analyzing all platforms"
            ;;
        *)
            echo -e "${RED}Invalid platform: $PLATFORM${NC}"
            echo "Valid platforms: posix, classic, all"
            exit 1
            ;;
    esac
}
print_header() {
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}Dead Code Detection Analysis${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
}

check_dependencies() {
    if ! command -v gcc &> /dev/null; then
        echo -e "${RED}Error: GCC is not installed.${NC}"
        exit 1
    fi
    
    # Detect Retro68 for Classic Mac support
    detect_retro68
    
    # Check for cppcheck if available
    if command -v cppcheck &> /dev/null; then
        HAVE_CPPCHECK=1
        echo -e "${GREEN}Found cppcheck - will include in analysis${NC}"
    else
        HAVE_CPPCHECK=0
        echo -e "${YELLOW}cppcheck not found - install for additional analysis${NC}"
        echo "  Install with: sudo apt install cppcheck"
    fi
    echo ""
}

cleanup() {
    rm -rf "$ANALYSIS_BUILD_DIR"
}

# Compile with maximum warnings to detect unused code
run_warning_analysis() {
    echo -e "${BLUE}=== Phase 1: Compiler Warning Analysis ===${NC}"
    echo "Compiling with extensive warning flags..."
    echo ""
    
    mkdir -p "$ANALYSIS_BUILD_DIR"
    mkdir -p "$REPORTS_DIR"
    
    # Comprehensive warning flags for dead code detection
    WARNING_FLAGS="-Wall -Wextra -Wunused -Wunused-function -Wunused-label \
                   -Wunused-value -Wunused-variable -Wunused-parameter \
                   -Wunused-but-set-parameter -Wunused-but-set-variable \
                   -Wunused-local-typedefs -Wunused-macros \
                   -Wunreachable-code -Wswitch-enum -Wswitch-default \
                   -Wundef -Wshadow -Wredundant-decls"
    
    # Additional flags for more analysis
    EXTRA_FLAGS="-fno-inline -fno-builtin -g"
    
    # Collect all warnings
    WARNINGS_FILE="$REPORTS_DIR/gcc_warnings.txt"
    > "$WARNINGS_FILE"
    
    # Find all C files
    for dir in $SOURCE_DIRS; do
        if [ -d "$dir" ]; then
            find "$dir" -name "*.c" -not -path "*/.finf/*" -not -path "*/.rsrc/*" | while read -r file; do
                # Skip DNR.c as it's third-party
                if [[ "$file" == *"DNR.c" ]]; then
                    continue
                fi
                
                echo "Analyzing: $file"
                # Get appropriate compiler and flags
                COMPILER=$(get_compiler_for_file "$file")
                INCLUDE_FLAGS=$(get_include_flags_for_file "$file")
                
                # Compile with warnings, capturing stderr
                $COMPILER $WARNING_FLAGS $EXTRA_FLAGS $INCLUDE_FLAGS \
                    -c "$file" -o "$ANALYSIS_BUILD_DIR/$(basename "$file").o" 2>&1 | \
                    grep -E "(warning:|error:)" | tee -a "$WARNINGS_FILE"
            done
        fi
    done
    
    # Summary of warnings
    echo ""
    echo -e "${YELLOW}Warning Summary:${NC}"
    echo "Total warnings: $(grep -c "warning:" "$WARNINGS_FILE" 2>/dev/null || echo 0)"
    grep -E "warning:" "$WARNINGS_FILE" 2>/dev/null | \
        sed 's/.*warning: //' | sort | uniq -c | sort -rn | head -20 || true
    echo ""
}

# Use nm to find potentially unused symbols
run_symbol_analysis() {
    echo -e "${BLUE}=== Phase 2: Symbol Analysis ===${NC}"
    echo "Analyzing symbols for potentially unused functions..."
    echo ""
    
    SYMBOLS_FILE="$REPORTS_DIR/unused_symbols.txt"
    > "$SYMBOLS_FILE"
    
    # First, compile all files to object files
    for dir in $SOURCE_DIRS; do
        if [ -d "$dir" ]; then
            find "$dir" -name "*.c" -not -path "*/.finf/*" -not -path "*/.rsrc/*" | while read -r file; do
                if [[ "$file" == *"DNR.c" ]]; then
                    continue
                fi
                # Get appropriate compiler and flags
                COMPILER=$(get_compiler_for_file "$file")
                INCLUDE_FLAGS=$(get_include_flags_for_file "$file")
                
                $COMPILER -c $INCLUDE_FLAGS "$file" \
                    -o "$ANALYSIS_BUILD_DIR/$(basename "$file").o" 2>/dev/null || true
            done
        fi
    done
    
    # Extract all defined functions (T = text/code symbols)
    echo "Defined functions:" >> "$SYMBOLS_FILE"
    find "$ANALYSIS_BUILD_DIR" -name "*.o" -exec nm -g {} \; 2>/dev/null | \
        grep " T " | awk '{print $3}' | sort | uniq >> "$SYMBOLS_FILE"
    
    # Extract all undefined/used functions (U = undefined symbols)
    echo -e "\nReferenced functions:" >> "$SYMBOLS_FILE"
    find "$ANALYSIS_BUILD_DIR" -name "*.o" -exec nm -g {} \; 2>/dev/null | \
        grep " U " | awk '{print $2}' | sort | uniq >> "$SYMBOLS_FILE"
    
    echo "Symbol analysis saved to: $SYMBOLS_FILE"
    echo ""
}

# Compile with function/data sections and garbage collection
run_section_analysis() {
    echo -e "${BLUE}=== Phase 3: Section-Based Dead Code Analysis ===${NC}"
    echo "Compiling with function sections and garbage collection..."
    echo ""
    
    # Create a simple test program that calls main functions
    cat > "$ANALYSIS_BUILD_DIR/test_main.c" << 'EOF'
// Dummy main to link against
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return 0;
}
EOF
    
    # Compile with sections and link with garbage collection
    SECTION_FLAGS="-ffunction-sections -fdata-sections"
    LINK_FLAGS="-Wl,--gc-sections -Wl,--print-gc-sections"
    
    GC_SECTIONS_FILE="$REPORTS_DIR/removed_sections.txt"
    
    echo "Attempting to link with garbage collection..." > "$GC_SECTIONS_FILE"
    
    # Try to compile and link all files together
    gcc $SECTION_FLAGS -I"posix" -I"shared" -I"classic_mac" \
        $(find $SOURCE_DIRS -name "*.c" -not -path "*/.finf/*" -not -path "*/.rsrc/*" | grep -v "DNR.c" | grep -v "main.c") \
        "$ANALYSIS_BUILD_DIR/test_main.c" \
        $LINK_FLAGS -o "$ANALYSIS_BUILD_DIR/test_app" 2>&1 | \
        grep "removing unused section" >> "$GC_SECTIONS_FILE" || true
    
    # Count removed functions
    REMOVED_COUNT=$(grep -c "removing unused section" "$GC_SECTIONS_FILE" 2>/dev/null || echo 0)
    echo "Functions/data removed by linker: $REMOVED_COUNT"
    echo ""
}

# Run cppcheck if available
run_cppcheck_analysis() {
    if [ $HAVE_CPPCHECK -eq 1 ]; then
        echo -e "${BLUE}=== Phase 4: Cppcheck Analysis ===${NC}"
        echo "Running cppcheck for additional dead code detection..."
        echo ""
        
        CPPCHECK_FILE="$REPORTS_DIR/cppcheck_results.txt"
        
        cppcheck --enable=all --inconclusive \
                 --suppress=missingIncludeSystem \
                 --suppress=unusedFunction \
                 -I posix -I shared -I classic_mac -I classic_mac_ot \
                 $SOURCE_DIRS 2> "$CPPCHECK_FILE"
        
        # Show summary
        echo "Cppcheck findings:"
        grep -E "(style|warning|error)" "$CPPCHECK_FILE" | head -20 || echo "No issues found"
        echo ""
    fi
}

# Generate final report
generate_report() {
    echo -e "${BLUE}=== Final Report ===${NC}"
    echo ""
    
    FINAL_REPORT="$REPORTS_DIR/deadcode_summary.txt"
    
    {
        echo "Dead Code Analysis Summary"
        echo "========================="
        echo "Generated: $(date)"
        echo ""
        
        echo "1. Unused Variables and Parameters"
        echo "---------------------------------"
        grep -E "unused (variable|parameter)" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | \
            head -10 || echo "None found"
        echo ""
        
        echo "2. Unused Functions"
        echo "------------------"
        grep -E "defined but not used" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | \
            head -10 || echo "None found"
        echo ""
        
        echo "3. Unreachable Code"
        echo "------------------"
        grep -E "unreachable" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | \
            head -10 || echo "None found"
        echo ""
        
        echo "4. Redundant Declarations"
        echo "------------------------"
        grep -E "redundant" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | \
            head -10 || echo "None found"
        echo ""
        
        if [ -f "$REPORTS_DIR/removed_sections.txt" ]; then
            echo "5. Functions Removed by Linker"
            echo "-----------------------------"
            grep "removing unused section" "$REPORTS_DIR/removed_sections.txt" 2>/dev/null | \
                sed 's/.*\.text\.//; s/'"'"'.*//g' | sort | uniq | head -20 || echo "None found"
        fi
        
    } > "$FINAL_REPORT"
    
    echo -e "${GREEN}Analysis complete!${NC}"
    echo ""
    echo "Reports generated in $REPORTS_DIR/:"
    echo "  - gcc_warnings.txt       : All compiler warnings"
    echo "  - unused_symbols.txt     : Symbol analysis"
    echo "  - removed_sections.txt   : Linker garbage collection"
    [ $HAVE_CPPCHECK -eq 1 ] && echo "  - cppcheck_results.txt   : Cppcheck analysis"
    echo "  - deadcode_summary.txt   : Summary report"
    echo ""
    echo "Key findings:"
    cat "$FINAL_REPORT" | grep -A 2 "^[0-9]\." | grep -v "^--$" || true
}

# --- Main script ---
main() {
    # Parse platform option if provided
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --platform|-p)
                PLATFORM="$2"
                shift 2
                ;;
            *)
                break
                ;;
        esac
    done
    
    print_header
    set_platform_dirs
    check_dependencies
    
    # Parse command line arguments
    case "${1:-full}" in
        "warnings")
            run_warning_analysis
            ;;
        "symbols")
            run_symbol_analysis
            ;;
        "sections")
            run_section_analysis
            ;;
        "full")
            run_warning_analysis
            run_symbol_analysis
            run_section_analysis
            run_cppcheck_analysis
            generate_report
            ;;
        "clean")
            cleanup
            rm -rf "$REPORTS_DIR"
            echo "Cleaned up analysis files"
            ;;
        "help"|"-h"|"--help")
            echo "Usage: $0 [--platform PLATFORM] [warnings|symbols|sections|full|clean|help]"
            echo ""
            echo "Platform Options:"
            echo "  --platform, -p PLATFORM  - Analyze specific platform"
            echo "                            PLATFORM can be: posix, classic, all (default: all)"
            echo ""
            echo "Analysis Options:"
            echo "  warnings - Run only compiler warning analysis"
            echo "  symbols  - Run only symbol analysis"
            echo "  sections - Run only section-based analysis"
            echo "  full     - Run all analyses (default)"
            echo "  clean    - Remove analysis files"
            echo "  help     - Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0                    # Analyze all platforms"
            echo "  $0 -p posix          # Analyze only POSIX code"
            echo "  $0 -p classic warnings  # Only warnings for Classic Mac"
            echo ""
            echo "The script uses GCC to detect:"
            echo "  - Unused variables, parameters, and functions"
            echo "  - Unreachable code"
            echo "  - Redundant declarations"
            echo "  - Dead code that can be eliminated"
            exit 0
            ;;
        *)
            echo -e "${RED}Invalid option: $1${NC}"
            echo "Use '$0 help' for usage information"
            exit 1
            ;;
    esac
    
    # Cleanup temporary files (keep reports)
    cleanup
}

# Run the main function
main "$@"