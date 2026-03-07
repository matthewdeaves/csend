#!/bin/bash
#
# autorun.sh — Unattended speckit.implement loop for Claude Code
#
# Auto-detects the highest-numbered spec with incomplete tasks and runs
# Claude Code in a loop to implement them. Designed to run overnight
# with hardware (Performa 6400) available for Classic Mac testing.
#
# Usage:
#   ./tools/autorun.sh              # Auto-detect feature, run from project root
#   ./tools/autorun.sh --feature=001-csend-app  # Explicit feature
#   ./tools/autorun.sh --dry-run    # Show what would happen without running Claude
#   ./tools/autorun.sh --resume     # Skip build check, resume after manual fix
#   ./tools/autorun.sh --notify     # Send desktop notification on completion
#

set -euo pipefail

# Allow running from within a Claude Code session
unset CLAUDECODE 2>/dev/null || true

# Detach from terminal stdin to prevent SIGTTIN when backgrounded.
if [ -t 0 ]; then
    exec < /dev/null
fi

# --- Auto-detect feature ---
auto_detect_feature() {
    local best_dir=""
    local best_num=0
    for dir in specs/[0-9]*; do
        [ -d "$dir" ] || continue
        local tasks="$dir/tasks.md"
        [ -f "$tasks" ] || continue
        local remaining
        remaining=$(grep -c '^\- \[ \]' "$tasks" 2>/dev/null) || true
        [ "${remaining:-0}" -gt 0 ] || continue
        local num
        num=$(basename "$dir" | grep -oE '^[0-9]+' | sed 's/^0*//')
        num="${num:-0}"
        if [ "$num" -gt "$best_num" ]; then
            best_num="$num"
            best_dir="$dir"
        fi
    done
    if [ -n "$best_dir" ]; then
        echo "$best_dir"
    fi
}

# --- Configuration ---
MAX_ITERATIONS=100
FEATURE_DIR=""
TASKS_FILE=""
COOLDOWN_SECONDS=15
RATE_LIMIT_WAIT_INITIAL=300
RATE_LIMIT_WAIT_MAX=3600
STUCK_THRESHOLD=3
BUILD_CHECK_AFTER=true
ITERATION_TIMEOUT=2700  # 45 minutes per iteration
DRY_RUN=false
RESUME=false
NOTIFY=false

# --- Parse arguments ---
for arg in "$@"; do
    case "$arg" in
        --dry-run)  DRY_RUN=true ;;
        --resume)   RESUME=true ;;
        --notify)   NOTIFY=true ;;
        --feature=*)
            FEATURE_DIR="specs/${arg#--feature=}"
            ;;
        --help|-h)
            echo "Usage: $0 [--dry-run] [--resume] [--notify] [--feature=NNN-name]"
            echo "  --dry-run          Show status without running Claude"
            echo "  --resume           Skip initial build check (after manual fix)"
            echo "  --notify           Send desktop notification on completion (Linux)"
            echo "  --feature=NAME     Use specific spec dir (e.g., --feature=001-csend-app)"
            echo ""
            echo "Without --feature, auto-detects the highest-numbered spec with incomplete tasks."
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg"
            exit 1
            ;;
    esac
done

# --- Resolve feature directory and tasks file ---
if [ -z "$FEATURE_DIR" ]; then
    FEATURE_DIR="$(auto_detect_feature)"
    if [ -z "$FEATURE_DIR" ]; then
        echo "ERROR: No spec directory with incomplete tasks found."
        echo "Hint: Use --feature=NNN-name to specify manually."
        exit 1
    fi
    echo "Auto-detected feature: $FEATURE_DIR"
fi

TASKS_FILE="$FEATURE_DIR/tasks.md"
FEATURE_NAME="$(basename "$FEATURE_DIR")"
export SPECIFY_FEATURE="$FEATURE_NAME"

# --- Ensure we're in project root ---
if [ ! -f "$TASKS_FILE" ]; then
    echo "ERROR: $TASKS_FILE not found. Run from project root."
    exit 1
fi

PROJECT_ROOT="$(pwd)"
CLOG_DIR="${CLOG_DIR:-$HOME/clog}"
PEERTALK_DIR="${PEERTALK_DIR:-$HOME/peertalk}"

# --- Preflight checks ---
if [ ! -f "$CLOG_DIR/build/libclog.a" ]; then
    echo "ERROR: clog not built. Expected $CLOG_DIR/build/libclog.a"
    echo "Build it first: cd $CLOG_DIR && mkdir -p build && cd build && cmake .. && make"
    exit 1
fi

if [ ! -d "$PEERTALK_DIR/include" ]; then
    echo "ERROR: peertalk not found. Expected $PEERTALK_DIR/include"
    echo "Clone it first: git clone https://github.com/matthewdeaves/peertalk.git $PEERTALK_DIR"
    exit 1
fi

# --- Setup logging ---
TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
LOG_DIR="logs/autorun-${TIMESTAMP}"
mkdir -p "$LOG_DIR"
SUMMARY_LOG="$LOG_DIR/summary.log"

log() {
    local msg="[$(date '+%Y-%m-%d %H:%M:%S')] $*"
    echo "$msg"
    echo "$msg" >> "$SUMMARY_LOG"
}

# --- Task counting helpers ---
count_done() {
    local n
    n=$(grep -c '^\- \[x\]' "$TASKS_FILE" 2>/dev/null) || true
    echo "${n:-0}"
}

count_remaining() {
    local n
    n=$(grep -c '^\- \[ \]' "$TASKS_FILE" 2>/dev/null) || true
    echo "${n:-0}"
}

current_phase() {
    grep -B15 '^\- \[ \]' "$TASKS_FILE" | grep '^## Phase' | head -1 || echo "Unknown"
}

next_incomplete_task() {
    grep '^\- \[ \]' "$TASKS_FILE" | head -1 | sed 's/- \[ \] //'
}

# --- Build verification ---
check_build() {
    if [ "$BUILD_CHECK_AFTER" = false ]; then
        return 0
    fi

    log "Verifying build..."
    mkdir -p build
    if (cd build && cmake .. -DCLOG_DIR="$CLOG_DIR" -DPEERTALK_DIR="$PEERTALK_DIR" 2>&1 && make 2>&1) > "$LOG_DIR/build-check-latest.log" 2>&1; then
        log "Build OK"
        return 0
    else
        log "BUILD BROKEN — see $LOG_DIR/build-check-latest.log"
        return 1
    fi
}

# --- Cross-build verification ---
check_all_builds() {
    local label="$1"
    local any_failed=false

    log "  Checking POSIX build..."
    mkdir -p build
    if ! (cd build && cmake .. -DCLOG_DIR="$CLOG_DIR" -DPEERTALK_DIR="$PEERTALK_DIR" 2>&1 && make 2>&1) > "$LOG_DIR/build-${label}-posix.log" 2>&1; then
        log "  WARNING: POSIX build failed — see $LOG_DIR/build-${label}-posix.log"
        any_failed=true
    else
        log "  POSIX build OK"
    fi

    for build_dir in build-ppc-ot build-ppc-mactcp build-68k; do
        if [ -d "$build_dir" ]; then
            log "  Checking $build_dir..."
            if ! (cd "$build_dir" && make 2>&1) > "$LOG_DIR/build-${label}-${build_dir}.log" 2>&1; then
                log "  WARNING: $build_dir build failed — see $LOG_DIR/build-${label}-${build_dir}.log"
                any_failed=true
            else
                log "  $build_dir build OK"
            fi
        fi
    done

    if [ "$any_failed" = true ]; then
        return 1
    fi
    return 0
}

# --- Notification ---
send_notification() {
    local title="$1"
    local body="$2"

    if [ "$NOTIFY" = true ]; then
        if command -v notify-send >/dev/null 2>&1; then
            notify-send "$title" "$body" 2>/dev/null || true
        fi
    fi
}

update_status_file() {
    local status="$1"
    mkdir -p logs
    echo "$status" > logs/autorun-latest-status.txt
}

# --- Git safety snapshot ---
git_snapshot() {
    local label="$1"
    local snap_file="$LOG_DIR/git-${label}.txt"
    {
        echo "=== git log -1 ==="
        git log --oneline -1 2>/dev/null || true
        echo ""
        echo "=== git diff --stat ==="
        git diff --stat 2>/dev/null || true
        echo ""
        echo "=== git diff ==="
        git diff 2>/dev/null || true
    } > "$snap_file"
    git log --oneline -1 >> "$SUMMARY_LOG" 2>/dev/null || true
}

# --- Machine connectivity check ---
check_machine() {
    local host="$1"
    local port="$2"
    if timeout 3 bash -c "echo >/dev/tcp/$host/$port" 2>/dev/null; then
        echo "reachable"
    else
        echo "unreachable"
    fi
}

get_machine_status() {
    local machines=""
    local p6400_ftp p6400_la p6200_ftp p6200_la macse_la
    local p6400_caps p6200_caps

    p6400_ftp="$(check_machine 10.188.1.102 21)"
    p6400_la="$(check_machine 10.188.1.102 1984)"
    if [ "$p6400_ftp" = "reachable" ] || [ "$p6400_la" = "reachable" ]; then
        p6400_caps=""
        [ "$p6400_ftp" = "reachable" ] && p6400_caps="FTP"
        [ "$p6400_la" = "reachable" ] && p6400_caps="${p6400_caps:+$p6400_caps + }LaunchAPPL"
        machines="${machines}- performa6400 (PPC/OT): ONLINE ($p6400_caps)\n"
    else
        machines="${machines}- performa6400 (PPC/OT): OFFLINE\n"
    fi

    p6200_ftp="$(check_machine 10.188.1.213 21)"
    p6200_la="$(check_machine 10.188.1.213 1984)"
    if [ "$p6200_ftp" = "reachable" ] || [ "$p6200_la" = "reachable" ]; then
        p6200_caps=""
        [ "$p6200_ftp" = "reachable" ] && p6200_caps="FTP"
        [ "$p6200_la" = "reachable" ] && p6200_caps="${p6200_caps:+$p6200_caps + }LaunchAPPL"
        machines="${machines}- performa6200 (PPC/MacTCP): ONLINE ($p6200_caps)\n"
    else
        machines="${machines}- performa6200 (PPC/MacTCP): OFFLINE\n"
    fi

    macse_la="$(check_machine 10.188.1.55 1984)"
    if [ "$macse_la" = "reachable" ]; then
        machines="${machines}- macse (68k/MacTCP): ONLINE (LaunchAPPL only)\n"
    else
        machines="${machines}- macse (68k/MacTCP): OFFLINE\n"
    fi

    printf "%b" "$machines"
}

# --- The prompt sent to Claude each iteration ---
build_prompt() {
    local done_count="$1"
    local remaining="$2"
    local next_task="$3"
    local machine_status
    machine_status="$(get_machine_status)"

    cat <<PROMPT
You are running in unattended automation mode. The following Classic Mac machines were tested for connectivity:

${machine_status}
Only attempt hardware tasks on machines marked ONLINE.

## Your job

Run /speckit.implement to execute tasks from ${TASKS_FILE}. Implement as many tasks as you can in this session, working through them in phase order.

## Current progress

- Tasks completed: ${done_count}
- Tasks remaining: ${remaining}
- Next incomplete task: ${next_task}

## Rules for this session

1. Run /speckit.implement — it will read the task list and work through them in order.
2. Implement tasks phase by phase. Complete all tasks in a phase before moving to the next.
3. After each task, verify the build compiles (mkdir -p build && cd build && cmake .. -DCLOG_DIR=\$HOME/clog -DPEERTALK_DIR=\$HOME/peertalk && make).
4. Mark completed tasks as done by changing \`- [ ]\` to \`- [x]\` in tasks.md.
5. If the build fails, fix the issue before moving on.
6. Commit after completing each phase (or more frequently if changes are large).
7. Do NOT skip tasks or work out of order.
8. For Classic Mac hardware testing tasks, use the MCP tools (execute_binary, upload_file, download_file) to deploy and run on real hardware.
9. The clog library is pre-built at ~/clog/build/libclog.a — use -DCLOG_DIR=\$HOME/clog in cmake.
10. The peertalk SDK is at ~/peertalk — use -DPEERTALK_DIR=\$HOME/peertalk in cmake.
11. Do NOT mark hardware tasks complete unless you actually executed on every machine listed in the task description.
12. **Feedback-first rule**: When a hardware test reveals unexpected platform behavior, run \`/speckit.feedback\` BEFORE implementing a fix.

## When finished

If ALL tasks in tasks.md are marked \`[x]\`, output the exact string: ALL_TASKS_COMPLETE
Otherwise, implement as far as you can and exit cleanly. The next session will pick up where you left off.
PROMPT
}

# --- Main ---
log "=========================================="
log "Autorun — Starting"
log "Project: $PROJECT_ROOT"
log "Tasks file: $TASKS_FILE"
log "Log dir: $LOG_DIR"
log "Max iterations: $MAX_ITERATIONS"
log "Iteration timeout: ${ITERATION_TIMEOUT}s"
log "=========================================="

DONE_BEFORE="$(count_done)"
REMAINING_BEFORE="$(count_remaining)"
log "Initial state: $DONE_BEFORE done, $REMAINING_BEFORE remaining"
log "Current phase: $(current_phase)"

if [ "$REMAINING_BEFORE" -eq 0 ]; then
    log "All tasks already complete. Nothing to do."
    update_status_file "COMPLETE: All tasks done"
    send_notification "Autorun" "All tasks already complete."
    exit 0
fi

if [ "$DRY_RUN" = true ]; then
    log ""
    log "Remaining tasks:"
    grep '^\- \[ \]' "$TASKS_FILE" | while IFS= read -r line; do
        log "  $line"
    done
    log ""
    log "Would run up to $MAX_ITERATIONS iterations to complete $REMAINING_BEFORE tasks."
    exit 0
fi

if [ "$RESUME" = false ]; then
    if [ -d "build" ]; then
        log "Running initial build check..."
        if ! check_build; then
            log "FATAL: Build is already broken before starting. Fix manually and use --resume."
            update_status_file "FAILED: Build broken at start"
            send_notification "Autorun FAILED" "Build broken before start."
            exit 1
        fi
    else
        log "No build directory yet — skipping initial build check."
    fi
fi

LAST_DONE_COUNT="$DONE_BEFORE"
STUCK_COUNT=0
RATE_LIMIT_CONSECUTIVE=0
ITERATIONS_USED=0

for i in $(seq 1 "$MAX_ITERATIONS"); do
    ITERATIONS_USED="$i"
    DONE_NOW="$(count_done)"
    REMAINING_NOW="$(count_remaining)"
    NEXT_TASK="$(next_incomplete_task)"

    log "------------------------------------------"
    log "Iteration $i/$MAX_ITERATIONS"
    log "Progress: $DONE_NOW/$((DONE_NOW + REMAINING_NOW)) done, $REMAINING_NOW remaining"
    log "Phase: $(current_phase)"
    log "Next: $NEXT_TASK"

    update_status_file "RUNNING: Iteration $i, $REMAINING_NOW remaining"

    if [ "$REMAINING_NOW" -eq 0 ]; then
        log "ALL TASKS COMPLETE"
        break
    fi

    if [ "$DONE_NOW" -eq "$LAST_DONE_COUNT" ] && [ "$i" -gt 1 ] && [ "$RATE_LIMIT_CONSECUTIVE" -eq 0 ]; then
        STUCK_COUNT=$((STUCK_COUNT + 1))
        log "WARNING: No progress after successful run (stuck count: $STUCK_COUNT/$STUCK_THRESHOLD)"
        if [ "$STUCK_COUNT" -ge "$STUCK_THRESHOLD" ]; then
            log "STUCK: Same task failed $STUCK_THRESHOLD times. Stopping."
            log "Last attempted task: $NEXT_TASK"
            update_status_file "STUCK: $NEXT_TASK failed $STUCK_THRESHOLD times"
            send_notification "Autorun STUCK" "Task failed $STUCK_THRESHOLD times: $NEXT_TASK"
            exit 1
        fi
    elif [ "$DONE_NOW" -gt "$LAST_DONE_COUNT" ]; then
        STUCK_COUNT=0
    fi
    LAST_DONE_COUNT="$DONE_NOW"

    git_snapshot "before-iter-$(printf '%03d' "$i")"

    PROMPT="$(build_prompt "$DONE_NOW" "$REMAINING_NOW" "$NEXT_TASK")"

    ITER_LOG="$LOG_DIR/iter-$(printf '%03d' "$i")-$(date +%H%M%S).log"
    log "Running Claude... (log: $ITER_LOG)"

    CLAUDE_EXIT=0
    if command -v stdbuf >/dev/null 2>&1; then
        timeout "$ITERATION_TIMEOUT" stdbuf -oL claude -p "$PROMPT" \
            --dangerously-skip-permissions \
            < /dev/null 2>&1 | tee "$ITER_LOG" || CLAUDE_EXIT=$?
    else
        timeout "$ITERATION_TIMEOUT" claude -p "$PROMPT" \
            --dangerously-skip-permissions \
            < /dev/null 2>&1 | tee "$ITER_LOG" || CLAUDE_EXIT=$?
    fi

    if [ "$CLAUDE_EXIT" -eq 124 ]; then
        log "TIMEOUT: Iteration $i killed after $((ITERATION_TIMEOUT / 60)) minutes"
        git_snapshot "after-iter-$(printf '%03d' "$i")"
        log "Cooling down ${COOLDOWN_SECONDS}s before next iteration..."
        sleep "$COOLDOWN_SECONDS"
        continue
    fi

    git_snapshot "after-iter-$(printf '%03d' "$i")"

    if grep -q "ALL_TASKS_COMPLETE" "$ITER_LOG"; then
        log "ALL TASKS COMPLETE (reported by Claude)"
        break
    fi

    if grep -qiE "rate.limit|usage.limit|capacity|overloaded|529|quota|too many|throttl" "$ITER_LOG" || [ "$CLAUDE_EXIT" -ne 0 ]; then
        RATE_LIMIT_CONSECUTIVE=$((RATE_LIMIT_CONSECUTIVE + 1))

        BACKOFF=$((RATE_LIMIT_WAIT_INITIAL * (2 ** (RATE_LIMIT_CONSECUTIVE - 1))))
        if [ "$BACKOFF" -gt "$RATE_LIMIT_WAIT_MAX" ]; then
            BACKOFF=$RATE_LIMIT_WAIT_MAX
        fi

        BACKOFF_MIN=$((BACKOFF / 60))
        RESUME_TIME="$(date -d "+${BACKOFF} seconds" '+%H:%M:%S' 2>/dev/null || date -v+${BACKOFF}S '+%H:%M:%S' 2>/dev/null || echo "unknown")"
        log "Rate limit or error (exit code: $CLAUDE_EXIT, consecutive: $RATE_LIMIT_CONSECUTIVE)"
        log "Backing off ${BACKOFF_MIN}m (resuming ~${RESUME_TIME})..."
        sleep "$BACKOFF"
    else
        RATE_LIMIT_CONSECUTIVE=0

        if [ "$BUILD_CHECK_AFTER" = true ]; then
            log "Running cross-build verification..."
            if ! check_all_builds "iter-$(printf '%03d' "$i")"; then
                log "WARNING: Some builds failed after iteration $i."
                log "Continuing — next iteration should fix it."
            fi
        fi

        log "Iteration $i complete. Cooling down ${COOLDOWN_SECONDS}s..."
        sleep "$COOLDOWN_SECONDS"
    fi
done

DONE_FINAL="$(count_done)"
REMAINING_FINAL="$(count_remaining)"
ELAPSED_TASKS=$((DONE_FINAL - DONE_BEFORE))

log "=========================================="
log "Autorun — Complete"
log "=========================================="
log "Tasks completed this run: $ELAPSED_TASKS"
log "Total done: $DONE_FINAL"
log "Remaining: $REMAINING_FINAL"
log "Iterations used: $ITERATIONS_USED"
log "Logs: $LOG_DIR"

if [ "$REMAINING_FINAL" -eq 0 ]; then
    log "SUCCESS: All tasks implemented."
    update_status_file "SUCCESS: All $DONE_FINAL tasks complete"
    send_notification "Autorun SUCCESS" "All $DONE_FINAL tasks complete in $ITERATIONS_USED iterations."
    exit 0
else
    log "INCOMPLETE: $REMAINING_FINAL tasks remain."
    update_status_file "INCOMPLETE: $REMAINING_FINAL tasks remain ($DONE_FINAL done)"
    send_notification "Autorun INCOMPLETE" "$REMAINING_FINAL tasks remain. $ELAPSED_TASKS completed this run."
    exit 2
fi
