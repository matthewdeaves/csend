# Log Filter Quick Reference

The `filter_logs.sh` script is designed to help debug CSend by extracting useful information from log files.

## Common Debugging Scenarios

### 1. Quick Overview
```bash
scripts/filter_logs.sh -s csend_posix.log
```
Shows: Total lines, log levels, top categories, peers detected, test results, connection issues

### 2. Find Problems
```bash
# Show only errors
scripts/filter_logs.sh -e csend_posix.log

# Show warnings and errors with context
scripts/filter_logs.sh -w -C 3 csend_posix.log

# Show all issues (errors + warnings) for specific peer
scripts/filter_logs.sh -w -p MacTCP csend_posix.log
```

### 3. Track Peer Activity
```bash
# All logs related to specific peer (non-debug)
scripts/filter_logs.sh -x -p OpenTransport@10.188.1.102 csend_posix.log

# Only networking logs for specific IP
scripts/filter_logs.sh -c NETWORKING -p 10.188.1.213 csend_posix.log
```

### 4. Analyze Test Results
```bash
# Show only test-related messages
scripts/filter_logs.sh -t csend_posix.log

# Show test messages with errors
scripts/filter_logs.sh -t -w csend_posix.log

# Show test round from specific time
scripts/filter_logs.sh -t -T "16:44:2" csend_posix.log
```

### 5. Focus on Categories
```bash
# Discovery protocol issues
scripts/filter_logs.sh -c DISCOVERY -i csend_posix.log

# Peer management (INFO and above)
scripts/filter_logs.sh -c PEER_MGMT -x csend_posix.log

# Messaging without debug noise
scripts/filter_logs.sh -c MESSAGING -w csend_posix.log
```

### 6. Time-based Analysis
```bash
# Show logs from specific time period
scripts/filter_logs.sh -T "16:44:2" csend_posix.log

# Last 50 lines of log
scripts/filter_logs.sh -n -50 csend_posix.log

# First 100 lines (startup sequence)
scripts/filter_logs.sh -n 100 csend_posix.log
```

### 7. Cross-Platform Comparison
```bash
# Compare test results across platforms
scripts/filter_logs.sh -t csend_posix.log > posix_test.txt
scripts/filter_logs.sh -t csend_mac.log > mactcp_test.txt
scripts/filter_logs.sh -t csend_classic_mac_ot_ppc.log > ot_test.txt
diff posix_test.txt mactcp_test.txt
```

### 8. Connection Debugging
```bash
# Show all connection-related errors with context
scripts/filter_logs.sh -e -c NETWORKING -C 5 csend_posix.log

# Track specific peer connection attempts
scripts/filter_logs.sh -p 10.188.1.213 -c NETWORKING csend_posix.log
```

## Filter Options Summary

| Option | Description | Example |
|--------|-------------|---------|
| `-s` | Summary statistics | `-s csend_posix.log` |
| `-e` | Errors only | `-e csend_posix.log` |
| `-w` | Warnings + Errors | `-w csend_posix.log` |
| `-i` | INFO + WARNING + ERROR | `-i csend_posix.log` |
| `-x` | Exclude DEBUG (INFO+) | `-x csend_posix.log` |
| `-c CAT` | Filter by category | `-c NETWORKING csend_posix.log` |
| `-p PEER` | Filter by peer | `-p MacTCP csend_posix.log` |
| `-t` | Test messages only | `-t csend_posix.log` |
| `-T PAT` | Time pattern | `-T "16:44:2" csend_posix.log` |
| `-C NUM` | Context lines | `-C 3 -e csend_posix.log` |
| `-n NUM` | First/last N lines | `-n 100` or `-n -50` |

## Categories Available

- `GENERAL` - General application events
- `NETWORKING` - TCP/UDP socket operations
- `DISCOVERY` - Peer discovery protocol
- `PEER_MGMT` - Peer list management
- `UI` - User interface events
- `PROTOCOL` - Protocol parsing/validation
- `SYSTEM` - System-level events
- `MESSAGING` - Message send/receive

## Tips for Debugging

1. **Start with summary** (`-s`) to understand overall log health
2. **Check errors first** (`-e`) to find critical issues
3. **Use context** (`-C 3`) to see what led to errors
4. **Filter by peer** (`-p`) when debugging specific connection issues
5. **Exclude DEBUG** (`-x`) to reduce noise in production logs
6. **Track tests** (`-t`) to verify messaging functionality
7. **Combine filters** for precise debugging (e.g., `-w -c NETWORKING -p MacTCP`)

## Example Debugging Workflow

```bash
# 1. Get overview
scripts/filter_logs.sh -s csend_posix.log

# 2. Check for errors
scripts/filter_logs.sh -e csend_posix.log

# 3. If connection issues found, investigate with context
scripts/filter_logs.sh -w -c NETWORKING -C 5 csend_posix.log

# 4. Track specific peer that's failing
scripts/filter_logs.sh -p 10.188.1.213 -x csend_posix.log

# 5. Compare with working peer
scripts/filter_logs.sh -p 10.188.1.102 -x csend_posix.log
```
