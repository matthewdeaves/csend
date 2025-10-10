#!/bin/bash

# Script to detect dead code, unused functions, and unreachable code using GCC
# Uses various GCC flags and compilation techniques with enhanced analysis

# --- Configuration ---
# Default: analyze all platforms
PLATFORM="all"
# Build directory for analysis
ANALYSIS_BUILD_DIR="build/deadcode_analysis"
# Reports directory
REPORTS_DIR="reports"
# Timestamp for this run
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
# Keep old reports flag (default: clean reports directory)
KEEP_REPORTS=false

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

    if [[ "$file" == classic_mac_mactcp/* ]] || [[ "$file" == classic_mac_ot/* ]]; then
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

    if [[ "$file" == classic_mac_mactcp/* ]] && [ -n "$RETRO68_PATH" ]; then
        # Use Retro68 includes for Classic Mac MacTCP files
        echo "-I\"$RETRO68_PATH/m68k-apple-macos/include\" -I\"$RETRO68_PATH/universal/CIncludes\" -Iclassic_mac_mactcp -Ishared -Ishared/classic_mac -Ishared/classic_mac/ui -D__MACOS__"
    elif [[ "$file" == classic_mac_ot/* ]] && [ -n "$RETRO68_PATH" ]; then
        # Use Retro68 includes for Classic Mac OpenTransport files
        echo "-I\"$RETRO68_PATH/m68k-apple-macos/include\" -I\"$RETRO68_PATH/universal/CIncludes\" -Iclassic_mac_ot -Ishared -Ishared/classic_mac -Ishared/classic_mac/ui -D__MACOS__ -DUSE_OPENTRANSPORT"
    else
        # Standard includes for POSIX
        echo "-Iposix -Ishared"
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
            SOURCE_DIRS="classic_mac_mactcp classic_mac_ot shared"
            echo "Analyzing Classic Mac platforms (MacTCP + OpenTransport)"
            ;;
        "all")
            SOURCE_DIRS="posix shared classic_mac_mactcp classic_mac_ot"
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

# Clean reports directory
clean_reports() {
    if [ "$KEEP_REPORTS" = false ]; then
        if [ -d "$REPORTS_DIR" ]; then
            echo -e "${BLUE}Cleaning reports directory...${NC}"
            rm -f "$REPORTS_DIR"/*.txt "$REPORTS_DIR"/*.html 2>/dev/null
            echo -e "${GREEN}Reports directory cleaned${NC}"
        fi
    else
        echo -e "${BLUE}Keeping old reports (--keep-reports flag set)${NC}"
    fi
    mkdir -p "$REPORTS_DIR"
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

    # Collect all warnings with enhanced context
    WARNINGS_FILE="$REPORTS_DIR/gcc_warnings.txt"
    DETAILED_WARNINGS="$REPORTS_DIR/gcc_warnings_detailed.txt"
    > "$WARNINGS_FILE"
    > "$DETAILED_WARNINGS"

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

                # Compile with warnings, capturing stderr with full context
                $COMPILER $WARNING_FLAGS $EXTRA_FLAGS $INCLUDE_FLAGS \
                    -c "$file" -o "$ANALYSIS_BUILD_DIR/$(basename "$file").o" 2>&1 | \
                    tee -a "$DETAILED_WARNINGS" | \
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
    # Only process directories that exist
    EXISTING_DIRS=""
    for dir in $SOURCE_DIRS; do
        if [ -d "$dir" ]; then
            EXISTING_DIRS="$EXISTING_DIRS $dir"
        fi
    done

    if [ -n "$EXISTING_DIRS" ]; then
        gcc $SECTION_FLAGS -I"posix" -I"shared" -I"classic_mac_mactcp" -I"classic_mac_ot" \
            $(find $EXISTING_DIRS -name "*.c" -not -path "*/.finf/*" -not -path "*/.rsrc/*" 2>/dev/null | grep -v "DNR.c" | grep -v "main.c") \
            "$ANALYSIS_BUILD_DIR/test_main.c" \
            $LINK_FLAGS -o "$ANALYSIS_BUILD_DIR/test_app" 2>&1 | \
            grep "removing unused section" >> "$GC_SECTIONS_FILE" || true
    fi
    
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
                 -I posix -I shared -I classic_mac_mactcp -I classic_mac_ot \
                 $SOURCE_DIRS 2> "$CPPCHECK_FILE"

        # Show summary
        echo "Cppcheck findings:"
        grep -E "(style|warning|error)" "$CPPCHECK_FILE" | head -20 || echo "No issues found"
        echo ""
    fi
}

# Analyze static functions specifically (HIGH confidence dead code)
analyze_static_functions() {
    echo -e "${BLUE}=== Phase 5: Static Function Analysis ===${NC}"
    echo "Identifying unused static functions (HIGH confidence dead code)..."
    echo ""

    STATIC_FUNCS_FILE="$REPORTS_DIR/static_functions_analysis.txt"
    > "$STATIC_FUNCS_FILE"

    echo "=== UNUSED STATIC FUNCTIONS (HIGH CONFIDENCE) ===" >> "$STATIC_FUNCS_FILE"
    echo "" >> "$STATIC_FUNCS_FILE"

    # Extract unused static function warnings with full context
    grep -E "defined but not used.*\[-Wunused-function\]" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | \
        grep "static" | while read -r line; do
        # Parse file:line:col: pattern
        if [[ "$line" =~ ^([^:]+):([0-9]+):[0-9]+:.*\'([^\']+)\' ]]; then
            file="${BASH_REMATCH[1]}"
            lineno="${BASH_REMATCH[2]}"
            funcname="${BASH_REMATCH[3]}"

            # Get git history for this file
            last_modified=$(git log -1 --format="%ai" -- "$file" 2>/dev/null || echo "Unknown")

            echo "Function: $funcname" >> "$STATIC_FUNCS_FILE"
            echo "  Location: $file:$lineno" >> "$STATIC_FUNCS_FILE"
            echo "  Last modified: $last_modified" >> "$STATIC_FUNCS_FILE"
            echo "  Confidence: HIGH (static function never called)" >> "$STATIC_FUNCS_FILE"
            echo "" >> "$STATIC_FUNCS_FILE"
        fi
    done

    COUNT=$(grep -c "^Function:" "$STATIC_FUNCS_FILE" 2>/dev/null || echo 0)
    echo "Found $COUNT unused static functions"
    echo ""
}

# Categorize findings by confidence level
categorize_by_confidence() {
    echo -e "${BLUE}=== Phase 6: Confidence Scoring ===${NC}"
    echo "Categorizing dead code by confidence level..."
    echo ""

    CONFIDENCE_REPORT="$REPORTS_DIR/confidence_report.txt"
    > "$CONFIDENCE_REPORT"

    {
        echo "=== DEAD CODE CONFIDENCE REPORT ==="
        echo "Generated: $(date)"
        echo ""

        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo "HIGH CONFIDENCE - Safe to remove"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo ""

        # Static unused functions
        echo "1. UNUSED STATIC FUNCTIONS"
        echo "   (defined but never called in same translation unit)"
        echo ""
        HIGH_COUNT=$(grep "defined but not used.*static" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | wc -l)
        if [ "$HIGH_COUNT" -gt 0 ]; then
            grep "defined but not used.*static" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | \
                sed 's/^/   /' || true
        else
            echo "   ✓ None found"
        fi
        echo ""

        # Unreachable code
        echo "2. UNREACHABLE CODE"
        echo "   (code that can never execute)"
        echo ""
        UNREACH_COUNT=$(grep "unreachable" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | wc -l)
        if [ "$UNREACH_COUNT" -gt 0 ]; then
            grep "unreachable" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | \
                sed 's/^/   /' || true
        else
            echo "   ✓ None found"
        fi
        echo ""

        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo "MEDIUM CONFIDENCE - Review before removing"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo ""

        # Unused variables
        echo "3. UNUSED VARIABLES"
        echo "   (local variables that are assigned but never read)"
        echo ""
        UNUSED_VAR=$(grep "unused variable" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | wc -l)
        if [ "$UNUSED_VAR" -gt 0 ]; then
            grep "unused variable" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | head -10 | \
                sed 's/^/   /' || true
            if [ "$UNUSED_VAR" -gt 10 ]; then
                echo "   ... and $((UNUSED_VAR - 10)) more"
            fi
        else
            echo "   ✓ None found"
        fi
        echo ""

        # Redundant declarations
        echo "4. REDUNDANT DECLARATIONS"
        echo "   (declarations that duplicate existing ones)"
        echo ""
        REDUNDANT=$(grep "redundant" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | wc -l)
        if [ "$REDUNDANT" -gt 0 ]; then
            grep "redundant" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | \
                sed 's/^/   /' || true
        else
            echo "   ✓ None found"
        fi
        echo ""

        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo "LOW CONFIDENCE - Consider interface requirements"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo ""

        # Unused parameters
        echo "5. UNUSED PARAMETERS"
        echo "   (may be required by callback/API signatures - use (void)param)"
        echo ""
        UNUSED_PARAM=$(grep "unused parameter" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | wc -l)
        if [ "$UNUSED_PARAM" -gt 0 ]; then
            grep "unused parameter" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | head -10 | \
                sed 's/^/   /' || true
            if [ "$UNUSED_PARAM" -gt 10 ]; then
                echo "   ... and $((UNUSED_PARAM - 10)) more"
            fi
        else
            echo "   ✓ None found"
        fi
        echo ""

        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo "SUMMARY"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo ""
        echo "HIGH confidence items:    $((HIGH_COUNT + UNREACH_COUNT))"
        echo "MEDIUM confidence items:  $((UNUSED_VAR + REDUNDANT))"
        echo "LOW confidence items:     $UNUSED_PARAM"
        echo ""

    } > "$CONFIDENCE_REPORT"

    echo "Confidence report generated"
    echo ""
}

# Add git history context to findings
add_git_context() {
    echo -e "${BLUE}=== Phase 7: Git History Analysis ===${NC}"
    echo "Adding git history context to findings..."
    echo ""

    GIT_CONTEXT_FILE="$REPORTS_DIR/git_context.txt"
    > "$GIT_CONTEXT_FILE"

    {
        echo "=== GIT HISTORY CONTEXT FOR DEAD CODE ==="
        echo "Generated: $(date)"
        echo ""

        # Extract unique files with warnings
        FILES_WITH_ISSUES=$(grep -E "^[^:]+\.c:" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | \
            cut -d':' -f1 | sort -u)

        for file in $FILES_WITH_ISSUES; do
            if [ -f "$file" ]; then
                echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
                echo "File: $file"
                echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

                # Last modification
                echo "Last modified: $(git log -1 --format='%ai' -- "$file" 2>/dev/null || echo 'Unknown')"
                echo "Last author:   $(git log -1 --format='%an' -- "$file" 2>/dev/null || echo 'Unknown')"
                echo "Last commit:   $(git log -1 --format='%h - %s' -- "$file" 2>/dev/null || echo 'Unknown')"
                echo ""

                # Issues in this file
                echo "Issues in this file:"
                grep "^$file:" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | sed 's/^/  /' || true
                echo ""
            fi
        done

    } > "$GIT_CONTEXT_FILE"

    echo "Git context analysis complete"
    echo ""
}

# Generate HTML report
generate_html_report() {
    echo -e "${BLUE}=== Phase 8: HTML Report Generation ===${NC}"
    echo "Generating interactive HTML report..."
    echo ""

    HTML_REPORT="$REPORTS_DIR/deadcode_report_${TIMESTAMP}.html"

    # Calculate counts first - HIGH confidence (true dead code)
    HIGH_STATIC=$(grep "defined but not used.*static" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | wc -l)
    HIGH_UNREACH=$(grep "unreachable" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | wc -l)
    HIGH_TOTAL=$((HIGH_STATIC + HIGH_UNREACH))

    # MEDIUM confidence (likely issues worth fixing)
    MEDIUM_VAR=$(grep "unused variable" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | wc -l)
    MEDIUM_REDUND=$(grep "redundant" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | wc -l)
    MEDIUM_SHADOW=$(grep "shadows a previous local" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | wc -l)
    MEDIUM_UNUSED_VAL=$(grep "value computed is not used" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | wc -l)
    MEDIUM_TOTAL=$((MEDIUM_VAR + MEDIUM_REDUND + MEDIUM_SHADOW + MEDIUM_UNUSED_VAL))

    # LOW confidence (API design choices, intentional patterns)
    LOW_PARAM=$(grep "unused parameter" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | wc -l)
    LOW_POINTER=$(grep "comparison between pointer and integer" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | wc -l)
    LOW_ENUM=$(grep "enumeration value.*not handled in switch" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | wc -l)
    LOW_CONV=$(grep -E "makes integer from pointer|incompatible pointer type" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | wc -l)
    LOW_TOTAL=$((LOW_PARAM + LOW_POINTER + LOW_ENUM + LOW_CONV))

    TOTAL_ISSUES=$((HIGH_TOTAL + MEDIUM_TOTAL + LOW_TOTAL))
    REPORT_DATE=$(date)

    {
        cat << HTML_HEADER
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Dead Code Analysis Report</title>
    <style>
HTML_HEADER
        cat << 'HTML_STYLE'
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            line-height: 1.6;
            color: #333;
            background: #f5f5f5;
            padding: 20px;
        }
        .container { max-width: 1400px; margin: 0 auto; background: white; padding: 30px; border-radius: 8px; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }
        h1 { color: #2c3e50; margin-bottom: 10px; border-bottom: 3px solid #3498db; padding-bottom: 10px; }
        h2 { color: #34495e; margin-top: 30px; margin-bottom: 15px; padding: 10px; background: #ecf0f1; border-left: 4px solid #3498db; }
        h3 { color: #7f8c8d; margin-top: 20px; margin-bottom: 10px; }
        .timestamp { color: #7f8c8d; font-size: 0.9em; margin-bottom: 20px; }
        .summary { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; margin: 20px 0; }
        .summary-card { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; padding: 20px; border-radius: 8px; }
        .summary-card.high { background: linear-gradient(135deg, #f093fb 0%, #f5576c 100%); }
        .summary-card.medium { background: linear-gradient(135deg, #fbc2eb 0%, #a6c1ee 100%); }
        .summary-card.low { background: linear-gradient(135deg, #a8edea 0%, #fed6e3 100%); }
        .summary-card h3 { color: white; margin: 0; font-size: 0.9em; }
        .summary-card .count { font-size: 2.5em; font-weight: bold; margin-top: 10px; }
        .filter-bar { margin: 20px 0; padding: 15px; background: #ecf0f1; border-radius: 8px; }
        .filter-bar input { padding: 10px; width: 300px; border: 1px solid #bdc3c7; border-radius: 4px; font-size: 14px; }
        .filter-bar select { padding: 10px; margin-left: 10px; border: 1px solid #bdc3c7; border-radius: 4px; font-size: 14px; }
        table { width: 100%; border-collapse: collapse; margin: 20px 0; }
        th { background: #34495e; color: white; padding: 12px; text-align: left; cursor: pointer; user-select: none; }
        th:hover { background: #2c3e50; }
        td { padding: 12px; border-bottom: 1px solid #ecf0f1; }
        tr:hover { background: #f8f9fa; }
        .confidence { display: inline-block; padding: 4px 12px; border-radius: 12px; font-size: 0.85em; font-weight: 600; }
        .confidence.high { background: #fee; color: #c33; }
        .confidence.medium { background: #ffe; color: #c93; }
        .confidence.low { background: #efe; color: #3c3; }
        .file-link { color: #3498db; text-decoration: none; font-family: monospace; }
        .file-link:hover { text-decoration: underline; }
        .issue-type { color: #e74c3c; font-weight: 600; }
        .git-info { font-size: 0.85em; color: #7f8c8d; font-style: italic; }
        pre { background: #2c3e50; color: #ecf0f1; padding: 15px; border-radius: 4px; overflow-x: auto; }
        .action-btn { background: #3498db; color: white; border: none; padding: 8px 16px; border-radius: 4px; cursor: pointer; margin: 5px; }
        .action-btn:hover { background: #2980b9; }
HTML_STYLE
        cat << HTML_BODY_START
    </style>
</head>
<body>
    <div class="container">
        <h1>🔍 Dead Code Analysis Report</h1>
        <div class="timestamp">Generated: $REPORT_DATE</div>

        <div class="summary">
            <div class="summary-card">
                <h3>Total Issues</h3>
                <div class="count">$TOTAL_ISSUES</div>
            </div>
            <div class="summary-card high">
                <h3>High Confidence</h3>
                <div class="count">$HIGH_TOTAL</div>
            </div>
            <div class="summary-card medium">
                <h3>Medium Confidence</h3>
                <div class="count">$MEDIUM_TOTAL</div>
            </div>
            <div class="summary-card low">
                <h3>Low Confidence</h3>
                <div class="count">$LOW_TOTAL</div>
            </div>
        </div>
HTML_BODY_START
        cat << 'HTML_TABLE_START'

        <div class="filter-bar">
            <input type="text" id="searchInput" placeholder="🔎 Search by file, function, or issue type..." onkeyup="filterTable()">
            <select id="confidenceFilter" onchange="filterTable()">
                <option value="">All Confidence Levels</option>
                <option value="high">High Confidence Only</option>
                <option value="medium">Medium Confidence Only</option>
                <option value="low">Low Confidence Only</option>
            </select>
        </div>

        <h2>📋 Detailed Findings</h2>
        <table id="issuesTable">
            <thead>
                <tr>
                    <th onclick="sortTable(0)">Confidence ▼</th>
                    <th onclick="sortTable(1)">File:Line ▼</th>
                    <th onclick="sortTable(2)">Issue Type ▼</th>
                    <th onclick="sortTable(3)">Description ▼</th>
                    <th onclick="sortTable(4)">Last Modified ▼</th>
                </tr>
            </thead>
            <tbody>
HTML_TABLE_START

        # Parse warnings and add to table
        grep -E "warning:" "$REPORTS_DIR/gcc_warnings.txt" 2>/dev/null | while IFS= read -r line; do
            # Parse: file:line:col: warning: message
            if [[ "$line" =~ ^([^:]+):([0-9]+):[0-9]+:.*warning:\ (.+)$ ]]; then
                file="${BASH_REMATCH[1]}"
                lineno="${BASH_REMATCH[2]}"
                message="${BASH_REMATCH[3]}"

                # Determine confidence and issue type
                confidence="low"
                issue_type="Other"

                if [[ "$message" =~ "defined but not used" ]] && [[ "$message" =~ "static" ]]; then
                    confidence="high"
                    issue_type="Unused Static Function"
                elif [[ "$message" =~ "unreachable" ]]; then
                    confidence="high"
                    issue_type="Unreachable Code"
                elif [[ "$message" =~ "unused variable" ]]; then
                    confidence="medium"
                    issue_type="Unused Variable"
                elif [[ "$message" =~ "redundant" ]]; then
                    confidence="medium"
                    issue_type="Redundant Declaration"
                elif [[ "$message" =~ "unused parameter" ]]; then
                    confidence="low"
                    issue_type="Unused Parameter"
                elif [[ "$message" =~ "shadows a previous local" ]]; then
                    confidence="medium"
                    issue_type="Shadow Variable"
                elif [[ "$message" =~ "value computed is not used" ]]; then
                    confidence="medium"
                    issue_type="Unused Value"
                elif [[ "$message" =~ "comparison between pointer and integer" ]]; then
                    confidence="low"
                    issue_type="Type Mismatch (API Design)"
                elif [[ "$message" =~ "enumeration value" ]] && [[ "$message" =~ "not handled in switch" ]]; then
                    confidence="low"
                    issue_type="Incomplete Switch (Intentional)"
                elif [[ "$message" =~ "makes integer from pointer" ]] || [[ "$message" =~ "incompatible pointer type" ]]; then
                    confidence="low"
                    issue_type="Type Conversion (API Design)"
                fi

                # Get git info
                git_date=$(git log -1 --format='%ai' -- "$file" 2>/dev/null | cut -d' ' -f1 || echo 'Unknown')

                # Clean up message
                clean_message=$(echo "$message" | sed "s/\[-W[^]]*\]//g" | sed 's/  */ /g')

                echo "                <tr data-confidence=\"$confidence\">"
                echo "                    <td><span class=\"confidence $confidence\">$confidence</span></td>"
                echo "                    <td><a href=\"#\" class=\"file-link\">$file:$lineno</a></td>"
                echo "                    <td><span class=\"issue-type\">$issue_type</span></td>"
                echo "                    <td>$clean_message</td>"
                echo "                    <td class=\"git-info\">$git_date</td>"
                echo "                </tr>"
            fi
        done

        cat << 'HTML_FOOTER'
            </tbody>
        </table>

        <h2>📊 Raw Reports</h2>
        <p>Detailed reports are available in the <code>reports/</code> directory:</p>
        <ul>
            <li><code>gcc_warnings.txt</code> - All compiler warnings</li>
            <li><code>confidence_report.txt</code> - Categorized by confidence level</li>
            <li><code>git_context.txt</code> - Git history context</li>
            <li><code>static_functions_analysis.txt</code> - Unused static functions</li>
        </ul>
    </div>

    <script>
        function filterTable() {
            const searchInput = document.getElementById('searchInput').value.toLowerCase();
            const confidenceFilter = document.getElementById('confidenceFilter').value;
            const table = document.getElementById('issuesTable');
            const rows = table.getElementsByTagName('tr');

            for (let i = 1; i < rows.length; i++) {
                const row = rows[i];
                const text = row.textContent.toLowerCase();
                const confidence = row.getAttribute('data-confidence');

                let showRow = true;

                if (searchInput && !text.includes(searchInput)) {
                    showRow = false;
                }

                if (confidenceFilter && confidence !== confidenceFilter) {
                    showRow = false;
                }

                row.style.display = showRow ? '' : 'none';
            }
        }

        function sortTable(columnIndex) {
            const table = document.getElementById('issuesTable');
            const rows = Array.from(table.rows).slice(1);
            const isAscending = table.rows[0].cells[columnIndex].textContent.includes('▼');

            rows.sort((a, b) => {
                const aVal = a.cells[columnIndex].textContent.trim();
                const bVal = b.cells[columnIndex].textContent.trim();

                if (isAscending) {
                    return aVal.localeCompare(bVal);
                } else {
                    return bVal.localeCompare(aVal);
                }
            });

            // Update arrow indicator
            const headers = table.rows[0].cells;
            for (let i = 0; i < headers.length; i++) {
                const text = headers[i].textContent.replace(/[▼▲]/g, '').trim();
                headers[i].textContent = text + (i === columnIndex ? (isAscending ? ' ▲' : ' ▼') : ' ▼');
            }

            // Reattach sorted rows
            rows.forEach(row => table.tBodies[0].appendChild(row));
        }
    </script>
</body>
</html>
HTML_FOOTER

    } > "$HTML_REPORT"

    echo "HTML report generated: $HTML_REPORT"
    echo ""
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
    echo "  - gcc_warnings.txt                : All compiler warnings"
    echo "  - gcc_warnings_detailed.txt       : Detailed warnings with full context"
    echo "  - unused_symbols.txt              : Symbol analysis"
    echo "  - removed_sections.txt            : Linker garbage collection"
    [ $HAVE_CPPCHECK -eq 1 ] && echo "  - cppcheck_results.txt            : Cppcheck analysis"
    echo "  - static_functions_analysis.txt   : Unused static functions (HIGH confidence)"
    echo "  - confidence_report.txt           : Categorized by confidence level"
    echo "  - git_context.txt                 : Git history context"
    echo "  - deadcode_report_${TIMESTAMP}.html : Interactive HTML report"
    echo "  - deadcode_summary.txt            : Summary report"
    echo ""
    echo "Key findings:"
    cat "$FINAL_REPORT" | grep -A 2 "^[0-9]\." | grep -v "^--$" || true
    echo ""
    echo -e "${GREEN}📊 Open the HTML report for interactive analysis:${NC}"
    echo "   file://$PWD/$REPORTS_DIR/deadcode_report_${TIMESTAMP}.html"
}

# --- Main script ---
main() {
    # Parse options
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --platform|-p)
                PLATFORM="$2"
                shift 2
                ;;
            --keep-reports|-k)
                KEEP_REPORTS=true
                shift
                ;;
            --help|-h|help)
                shift
                set -- "help"
                break
                ;;
            *)
                break
                ;;
        esac
    done

    print_header
    set_platform_dirs
    check_dependencies
    clean_reports
    
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
            analyze_static_functions
            categorize_by_confidence
            add_git_context
            generate_html_report
            generate_report
            ;;
        "clean")
            cleanup
            rm -rf "$REPORTS_DIR"
            echo "Cleaned up analysis files"
            ;;
        "help"|"-h"|"--help")
            echo "Usage: $0 [OPTIONS] [MODE]"
            echo ""
            echo "Options:"
            echo "  --platform, -p PLATFORM  - Analyze specific platform (posix, classic, all)"
            echo "                             Default: all"
            echo "  --keep-reports, -k       - Keep old reports (don't clean reports directory)"
            echo "                             Default: clean reports before analysis"
            echo "  --help, -h               - Show this help message"
            echo ""
            echo "Analysis Modes:"
            echo "  warnings - Run only compiler warning analysis"
            echo "  symbols  - Run only symbol analysis"
            echo "  sections - Run only section-based analysis"
            echo "  full     - Run all analyses including advanced features (default)"
            echo "  clean    - Remove analysis and report files"
            echo "  help     - Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0                        # Full analysis, clean reports"
            echo "  $0 --keep-reports         # Full analysis, keep old reports"
            echo "  $0 -p posix               # Analyze only POSIX code"
            echo "  $0 -p classic -k warnings # Classic Mac warnings, keep old reports"
            echo ""
            echo "Enhanced Features (full mode only):"
            echo "  ✓ Confidence scoring (HIGH/MEDIUM/LOW)"
            echo "  ✓ Static function analysis"
            echo "  ✓ Git history integration"
            echo "  ✓ Interactive HTML report with filtering/sorting"
            echo ""
            echo "The script detects:"
            echo "  - Dead code (unused functions, unreachable code)"
            echo "  - Code quality issues (shadow variables, unused values)"
            echo "  - API design patterns (type mismatches, incomplete switches)"
            echo ""
            echo "Reports Directory:"
            echo "  By default, old reports are cleaned before analysis."
            echo "  Use --keep-reports to preserve historical reports."
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