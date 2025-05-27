# Logging System Migration Guide

## Overview
The CSend logging system supports log levels (ERROR, WARNING, INFO, DEBUG) and categories for better filtering and debugging.

## Log Levels
- **ERROR**: Critical failures, resource allocation failures
- **WARNING**: Non-critical issues, degraded conditions
- **INFO**: Important state changes, connections, milestones
- **DEBUG**: Detailed tracing, routine operations

## Categories
- **LOG_CAT_GENERAL**: Default category for uncategorized logs
- **LOG_CAT_NETWORKING**: TCP/UDP operations, sockets
- **LOG_CAT_DISCOVERY**: Peer discovery broadcasts/responses
- **LOG_CAT_PEER_MGMT**: Peer list management
- **LOG_CAT_UI**: User interface operations
- **LOG_CAT_PROTOCOL**: Message parsing/formatting
- **LOG_CAT_SYSTEM**: Thread management, application lifecycle
- **LOG_CAT_MESSAGING**: Message sending/receiving

### Example Logging
All logging functions require a category to be specified:
```c
log_error_cat(LOG_CAT_NETWORKING, "Failed to connect to %s:%d - %s", ip, port, strerror(errno));
log_info_cat(LOG_CAT_NETWORKING, "TCP listener initialized on port %d", PORT_TCP);
log_info_cat(LOG_CAT_NETWORKING, "Accepted connection from %s", sender_ip);
log_debug_cat(LOG_CAT_SYSTEM, "Starting listener thread");
```

Note: There are no convenience functions without categories - you must always specify which subsystem you're logging from.

## Log Output Format
```
2024-01-15 10:23:45 [ERROR][NETWORKING] Failed to connect to 192.168.1.1:8080
2024-01-15 10:23:46 [INFO][DISCOVERY] New peer discovered: alice@192.168.1.2
2024-01-15 10:23:47 [DEBUG][UI] Button click detected: Send
```

## Filtering Logs
Use the provided `filter_logs.sh` script:

```bash
# Show only errors
./filter_logs.sh -e app_posix.log

# Show warnings and above
./filter_logs.sh -w app_posix.log

# Show only networking logs
./filter_logs.sh -c NETWORKING app_posix.log

# Show INFO and above for discovery
./filter_logs.sh -l INFO -c DISCOVERY app_posix.log
```

## Setting Log Level Programmatically
```c
// Only show warnings and errors
set_log_level(LOG_LEVEL_WARNING);

// Show everything (default)
set_log_level(LOG_LEVEL_DEBUG);
```