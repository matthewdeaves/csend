# Feature Specification: Cppcheck Code Quality Improvements

**Feature Branch**: `003-cppcheck-improvements`
**Created**: 2026-03-07
**Status**: Draft
**Input**: User description: "Fix cppcheck code quality issues including const qualifiers, variable scope reductions, and unused variable cleanup"

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Code Passes Static Analysis (Priority: P1)

A developer runs cppcheck with all checks enabled on the codebase and expects zero style warnings. This ensures the code follows best practices for const-correctness, minimal variable scope, and no dead code.

**Why this priority**: Clean static analysis results indicate maintainable, high-quality code and prevent future bugs from accumulating.

**Independent Test**: Run `cppcheck --enable=all --suppress=missingIncludeSystem --suppress=unusedFunction --suppress=missingInclude posix/ shared/ classic_mac/ 2>&1 | grep -E "(style|warning|error):"` and verify no output.

**Acceptance Scenarios**:

1. **Given** the codebase with current issues, **When** all fixes are applied, **Then** cppcheck reports zero style/warning/error issues
2. **Given** const qualifier fixes are applied, **When** code is compiled, **Then** no compiler errors occur and behavior is unchanged

---

### User Story 2 - Const-Correct Pointer Variables (Priority: P2)

Variables that point to data which is never modified should be declared as `const` pointers. This communicates intent and allows compiler optimizations.

**Why this priority**: Const-correctness improves code safety and documentation without changing behavior.

**Independent Test**: Verify each `tm_info` pointer variable is declared as `const struct tm *` and code compiles successfully.

**Acceptance Scenarios**:

1. **Given** `localtime()` or `gmtime()` returns a pointer, **When** the result is stored, **Then** the variable is declared as `const struct tm *`
2. **Given** const pointer declarations, **When** code is compiled, **Then** no "discards const qualifier" warnings appear

---

### User Story 3 - Const-Correct Function Parameters (Priority: P2)

Function parameters that are not modified within the function body should be declared as const. This prevents accidental modification and documents the function contract.

**Why this priority**: Const parameters make function contracts explicit and catch bugs at compile time.

**Independent Test**: Verify affected parameters are const-qualified and code compiles without warnings.

**Acceptance Scenarios**:

1. **Given** `main(int argc, char *argv[])`, **When** argv is not modified, **Then** declare as `char * const argv[]`
2. **Given** `EventRecord *theEvent` parameters, **When** the event record is only read, **Then** declare as `const EventRecord *theEvent`

---

### User Story 4 - Minimal Variable Scope (Priority: P3)

Variables should be declared in the smallest scope where they are used. This reduces cognitive load and potential for errors.

**Why this priority**: Reducing scope is a style improvement that aids readability but doesn't affect functionality.

**Independent Test**: Verify variables are moved to their minimal required scope and code compiles successfully.

**Acceptance Scenarios**:

1. **Given** a variable declared at function scope but only used in one block, **When** the fix is applied, **Then** the variable is declared within that block
2. **Given** Classic Mac C89 constraints, **When** moving variable declarations, **Then** declarations remain at the start of their enclosing block (not inline with code)

---

### User Story 5 - Remove Unused Variables (Priority: P3)

Variables that are assigned but never read are dead code that should be removed or the logic should be corrected.

**Why this priority**: Dead code removal is straightforward cleanup with no functional impact.

**Independent Test**: Verify unused variables are either removed or their values are used appropriately.

**Acceptance Scenarios**:

1. **Given** `currentScrollVal` and `maxScroll` in dialog_messages.c, **When** analyzed, **Then** either remove the variables or use their values appropriately
2. **Given** the variables are used to compute `scrolledToBottom`, **When** the fix is applied, **Then** the intermediate variables are eliminated by computing the condition directly

---

### Edge Cases

- **C89 variable declarations**: When reducing variable scope in Classic Mac code, declarations must remain at the start of the enclosing block (not inline with statements). This is handled by moving declarations to the top of inner blocks rather than declaring at point of use.
- **Toolbox callback signatures**: The `EventRecord` struct in Classic Mac Toolbox callbacks accepts const pointers without breaking compatibility, as these functions only read from the event record.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Code MUST pass cppcheck style checks with the suppression flags: `--suppress=missingIncludeSystem --suppress=unusedFunction --suppress=missingInclude`
- **FR-002**: All `struct tm *` pointers from `localtime()` and `gmtime()` MUST be declared as `const struct tm *`
- **FR-003**: Function parameters that are read-only MUST be declared const where API-compatible
- **FR-004**: Variables MUST be declared in the smallest scope where they are used
- **FR-005**: Variables that are assigned but never read MUST be removed or utilized
- **FR-006**: All changes MUST maintain C89 compatibility for Classic Mac builds (variable declarations at block start)
- **FR-007**: All changes MUST compile without warnings on both POSIX and Classic Mac targets

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Running cppcheck with specified flags produces zero style/warning/error output
- **SC-002**: POSIX build completes without new compiler warnings
- **SC-003**: Classic Mac 68k build completes without new compiler warnings
- **SC-004**: All existing tests continue to pass after changes

## Assumptions

- The cppcheck version available provides accurate analysis for C89/C99 code
- Classic Mac builds require C89-style variable declarations (at block start, not inline)
- The `EventRecord` struct in Classic Mac Toolbox can accept const pointers without breaking callbacks
- Changes to `main()` signature (`char * const argv[]`) are compatible with POSIX standards
- No automated test suite exists for csend; SC-004 is satisfied by successful builds and manual verification that chat functionality works
