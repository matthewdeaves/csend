# Tasks: Cppcheck Code Quality Improvements

**Input**: Design documents from `/specs/003-cppcheck-improvements/`
**Prerequisites**: plan.md (required), spec.md (required for user stories)

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel (different files, no dependencies)
- **[Story]**: Which user story this task belongs to (e.g., US1, US2, US3)
- Include exact file paths in descriptions

---

## Phase 1: Setup

**Purpose**: No setup required - this is a refactoring task on existing code

*No tasks in this phase*

---

## Phase 2: User Story 2 - Const-Correct Pointer Variables (Priority: P2)

**Goal**: Add const qualifiers to `struct tm *` pointer variables returned from `localtime()` and `gmtime()`

**Independent Test**: Code compiles without warnings and cppcheck no longer reports constVariablePointer issues

### Implementation

- [x] T001 [P] [US2] Change `struct tm *tm_info` to `const struct tm *tm_info` in posix/ui_terminal_interactive.c:12
- [x] T002 [P] [US2] Change `struct tm *tm_info` to `const struct tm *tm_info` in posix/ui_terminal_machine.c:42
- [x] T003 [P] [US2] Change `struct tm *tm_info` to `const struct tm *tm_info` in posix/ui_terminal_machine.c:479

**Checkpoint**: All tm_info pointers are const-qualified

---

## Phase 3: User Story 3 - Const-Correct Function Parameters (Priority: P2)

**Goal**: Add const qualifiers to function parameters that are read-only

**Independent Test**: Code compiles without warnings and cppcheck no longer reports constParameter/constParameterPointer issues

### Implementation

- [x] T004 [P] [US3] Change `char *argv[]` to `char * const argv[]` in posix/main.c:33
- [x] T005 [P] [US3] Change `EventRecord *theEvent` to `const EventRecord *theEvent` in shared/classic_mac/ui/dialog_input.c:130 (HandleInputTEClick)
- [x] T006 [P] [US3] Change `EventRecord *theEvent` to `const EventRecord *theEvent` in shared/classic_mac/ui/dialog_input.c:535 (HandleInputTEKeyDown)

**Checkpoint**: All read-only parameters are const-qualified

---

## Phase 4: User Story 4 - Reduce Variable Scope (Priority: P3)

**Goal**: Move variable declarations to the smallest scope where they are used

**Independent Test**: Code compiles without warnings and cppcheck no longer reports variableScope issues

### Implementation

- [x] T007 [P] [US4] Move `theChar` declaration from line 537 into the block where it's used in shared/classic_mac/ui/dialog_input.c
- [x] T008 [P] [US4] Move `connectedIdx` declaration from line 138 into the if-block where it's used in shared/classic_mac/ui/dialog_peerlist.c

**Checkpoint**: Variables are declared in minimal scope (C89 compatible)

---

## Phase 5: User Story 5 - Remove Unused Variables (Priority: P3)

**Goal**: Remove or properly utilize variables that are assigned but never read

**Independent Test**: Code compiles without warnings and cppcheck no longer reports unreadVariable issues

### Implementation

- [x] T009 [US5] Inline `currentScrollVal` and `maxScroll` assignments in shared/classic_mac/ui/dialog_messages.c:120-126 - compute `scrolledToBottom` directly without intermediate variables

**Checkpoint**: No unused variable assignments remain

---

## Phase 6: Verification (User Story 1 - Code Passes Static Analysis)

**Purpose**: Verify all issues are resolved and US1 acceptance criteria are met

- [x] T010 [US1] Run cppcheck verification: `cppcheck --enable=all --suppress=missingIncludeSystem --suppress=unusedFunction --suppress=missingInclude posix/ shared/ classic_mac/ 2>&1 | grep -E "(style|warning|error):"` and confirm no output
- [x] T011 [US1] Build POSIX target: `cmake -B build && cmake --build build` and verify no new warnings
- [x] T012 [US1] Build 68k MacTCP target (if Retro68 available): verify no new warnings

---

## Dependencies & Execution Order

### Phase Dependencies

- **Phase 2-5 (User Stories)**: All independent - can execute in any order or in parallel
- **Phase 6 (Verification)**: Depends on all user story phases being complete

### Parallel Opportunities

All tasks T001-T009 are independent (different files or different lines) and can be executed in parallel.

```bash
# All const pointer variable fixes can run in parallel:
T001, T002, T003

# All const parameter fixes can run in parallel:
T004, T005, T006

# All variable scope fixes can run in parallel:
T007, T008

# Then the unused variable fix:
T009
```

---

## Implementation Strategy

### Recommended Approach

1. Apply all const qualifier fixes first (T001-T006) - lowest risk
2. Apply variable scope fixes (T007-T008) - straightforward moves
3. Apply unused variable fix (T009) - requires understanding the logic
4. Run verification (T010-T012)

### Single-Pass Approach

Since all changes are independent, they can all be applied in a single pass:
1. Edit all 6 files with the specified changes
2. Run cppcheck to verify
3. Build both targets to confirm no regressions

---

## Summary

| Phase | User Story | Task Count | Description |
|-------|------------|------------|-------------|
| 2 | US2 | 3 | Const pointer variables |
| 3 | US3 | 3 | Const function parameters |
| 4 | US4 | 2 | Variable scope reduction |
| 5 | US5 | 1 | Unused variable removal |
| 6 | US1 | 3 | Verification (static analysis pass) |
| **Total** | | **12** | |

---

## Notes

- All changes must maintain C89 compatibility (variable declarations at block start)
- Classic Mac Toolbox `EventRecord` pointer should accept const qualifier
- Changes are purely mechanical refactoring with no behavioral impact
