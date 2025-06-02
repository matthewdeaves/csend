# 📋 CSend Logging System

<div align="center">

*Comprehensive logging with categories, levels, and cross-platform support*

[![Log Levels](https://img.shields.io/badge/Log%20Levels-4-blue?style=flat-square)](#log-levels)
[![Categories](https://img.shields.io/badge/Categories-8-green?style=flat-square)](#categories)
[![Platforms](https://img.shields.io/badge/Platforms-POSIX%20%7C%20Classic%20Mac-orange?style=flat-square)](#platform-differences)

</div>

---

## 🎯 Overview

The CSend logging system provides structured logging with **4 severity levels** and **8 categories** for effective debugging and monitoring across both POSIX and Classic Mac platforms. All logging requires explicit categorization for better organization and filtering.

---

## 📊 Log Levels

CSend uses a hierarchical logging system where each level includes all levels above it:

| Level | Value | Description | When to Use |
|-------|-------|-------------|-------------|
| 🔴 **ERROR** | 0 | Critical failures | Resource allocation failures, network errors, system crashes |
| 🟡 **WARNING** | 1 | Non-critical issues | Degraded conditions, recoverable errors, performance issues |
| 🔵 **INFO** | 2 | Important events | State changes, connections, application milestones |
| 🟢 **DEBUG** | 3 | Detailed tracing | Routine operations, function entry/exit, detailed flow |

> **Default Level**: `DEBUG` (shows all messages)

---

## 🏷️ Categories

Every log message must specify a category for better organization:

| Category | Purpose | Examples |
|----------|---------|----------|
| **GENERAL** | Default/uncategorized | Application startup, general errors |
| **NETWORKING** | TCP/UDP operations | Socket creation, connection events, network errors |
| **DISCOVERY** | Peer discovery | UDP broadcasts, peer responses, discovery timeouts |
| **PEER_MGMT** | Peer list management | Adding/removing peers, peer status updates |
| **UI** | User interface | Button clicks, window events, user interactions |
| **PROTOCOL** | Message parsing/formatting | Message validation, protocol errors |
| **SYSTEM** | Application lifecycle | Thread management, initialization, shutdown |
| **MESSAGING** | Message operations | Sending/receiving messages, message queuing |

---

## 🔧 API Reference

### Core Logging Functions

All logging functions require a category parameter:

```c
// Categorized logging functions
log_error_cat(log_category_t category, const char *format, ...);
log_warning_cat(log_category_t category, const char *format, ...);
log_info_cat(log_category_t category, const char *format, ...);
log_debug_cat(log_category_t category, const char *format, ...);

// Special purpose function (no level/category)
log_app_event(const char *format, ...);
```

### Configuration Functions

```c
// Initialize logging system
log_init(const char *log_file_name_suggestion, platform_logging_callbacks_t *callbacks);

// Shutdown logging system
log_shutdown(void);

// Control debug output to screen/UI
set_debug_output_enabled(Boolean enabled);
Boolean is_debug_output_enabled(void);

// Set minimum log level
set_log_level(log_level_t level);
log_level_t get_log_level(void);
```

---

## 💻 Usage Examples

### Basic Logging

```c
// Network operations
log_error_cat(LOG_CAT_NETWORKING, "Failed to connect to %s:%d - %s", 
              ip, port, strerror(errno));
log_info_cat(LOG_CAT_NETWORKING, "TCP listener initialized on port %d", PORT_TCP);
log_debug_cat(LOG_CAT_NETWORKING, "Accepted connection from %s", sender_ip);

// Peer discovery
log_info_cat(LOG_CAT_DISCOVERY, "New peer discovered: %s@%s", username, ip);
log_warning_cat(LOG_CAT_DISCOVERY, "Discovery timeout for peer %s", ip);

// System operations
log_debug_cat(LOG_CAT_SYSTEM, "Starting listener thread");
log_info_cat(LOG_CAT_SYSTEM, "Application shutdown initiated");

// User interface (Classic Mac)
log_debug_cat(LOG_CAT_UI, "Button click detected: Send");
log_info_cat(LOG_CAT_UI, "Dialog window initialized");
```

### Setting Log Levels

```c
// Only show warnings and errors
set_log_level(LOG_LEVEL_WARNING);

// Show everything (default)
set_log_level(LOG_LEVEL_DEBUG);

// Show only critical errors
set_log_level(LOG_LEVEL_ERROR);
```

### Debug Output Control

```c
// Enable debug output to screen/UI (disabled by default)
set_debug_output_enabled(true);

// Check current state
if (is_debug_output_enabled()) {
    log_debug_cat(LOG_CAT_SYSTEM, "Debug output is enabled");
}
```

---

## 📁 Log Files

### File Naming

| Platform | Default File Name | Fallback Name |
|----------|------------------|---------------|
| **POSIX** | `csend_posix.log` | `app_posix.log` |
| **Classic Mac** | `csend_mac.log` | `app_classic_mac.log` |

### Output Format

**Standard Log Entry:**
```
YYYY-MM-DD HH:MM:SS [LEVEL][CATEGORY] message content
```

**Examples:**
```
2024-01-15 10:23:45 [ERROR][NETWORKING] Failed to connect to 192.168.1.1:2555
2024-01-15 10:23:46 [INFO][DISCOVERY] New peer discovered: alice@192.168.1.2
2024-01-15 10:23:47 [DEBUG][UI] Button click detected: Send
2024-01-15 10:23:48 [WARNING][PEER_MGMT] Peer timeout: bob@192.168.1.3
```

**App Event Format:**
```
2024-01-15 10:23:49 User 'alice' joined the chat
```

---

## 🔍 Log Filtering

Use the `filter_logs.sh` script for advanced log analysis:

### Filter by Level

```bash
# Show only errors
./filter_logs.sh -e csend_posix.log

# Show warnings and above
./filter_logs.sh -w csend_posix.log

# Show info and above
./filter_logs.sh -i csend_posix.log

# Specify exact level
./filter_logs.sh -l DEBUG csend_posix.log
```

### Filter by Category

```bash
# Show only networking logs
./filter_logs.sh -c NETWORKING csend_posix.log

# Show only discovery logs
./filter_logs.sh -c DISCOVERY csend_posix.log

# Show only UI interactions
./filter_logs.sh -c UI csend_posix.log
```

### Combined Filters

```bash
# Show INFO and above for discovery category
./filter_logs.sh -l INFO -c DISCOVERY csend_posix.log

# Show only networking errors
./filter_logs.sh -e -c NETWORKING csend_posix.log
```

### Filter Options Reference

| Option | Description | Example |
|--------|-------------|---------|
| `-l LEVEL` | Show logs at or above specified level | `-l WARNING` |
| `-c CATEGORY` | Show only logs from specific category | `-c NETWORKING` |
| `-e` | Show only ERROR level logs | `-e` |
| `-w` | Show WARNING and ERROR logs | `-w` |
| `-i` | Show INFO, WARNING and ERROR logs | `-i` |

---

## 🎛️ Runtime Control

### In-Application Commands

**POSIX Terminal UI:**
```
/debug          # Toggle debug output on/off
```

**Classic Mac GUI:**
- Use the "Show Debug" checkbox in the dialog

### Debug Output Behavior

| State | Log File | Screen/UI Output |
|-------|----------|------------------|
| **Disabled** (default) | ✅ All levels | ❌ None |
| **Enabled** | ✅ All levels | ✅ All levels |

---

## 🖥️ Platform Differences

### POSIX Implementation

- **Timestamp**: Uses `strftime()` with system time
- **Debug Output**: Printed to `stdout` via `printf()`
- **Thread Safety**: Full thread-safe logging
- **String Formatting**: `vsnprintf()` (safe, prevents buffer overflows)

### Classic Mac Implementation

- **Timestamp**: Uses `GetTime()` with manual formatting
- **Debug Output**: Appended to TextEdit control in GUI window
- **Threading**: Single-threaded, event-driven
- **String Formatting**: `vsprintf()` (Classic Mac compatible)
- **Reentrancy**: Protected against recursive logging calls

---

## 🚀 Quick Start

### 1. Initialize Logging

**POSIX:**
```c
platform_logging_callbacks_t posix_callbacks = {
    .get_timestamp = posix_platform_get_timestamp,
    .display_debug_log = posix_platform_display_debug_log
};
log_init("csend_posix.log", &posix_callbacks);
```

**Classic Mac:**
```c
platform_logging_callbacks_t mac_callbacks = {
    .get_timestamp = classic_mac_platform_get_timestamp,
    .display_debug_log = classic_mac_platform_display_debug_log
};
log_init("csend_mac.log", &mac_callbacks);
```

### 2. Start Logging

```c
log_info_cat(LOG_CAT_SYSTEM, "Application started successfully");
```

### 3. Enable Debug Output (Optional)

```c
set_debug_output_enabled(true);
```

### 4. Shutdown

```c
log_shutdown();
```

---

## 📚 Best Practices

1. **Always Use Categories**: Never log without specifying a category
2. **Choose Appropriate Levels**: Use ERROR for critical issues, DEBUG for detailed tracing
3. **Include Context**: Add relevant variables and state information
4. **Consistent Formatting**: Use clear, descriptive messages
5. **Filter in Production**: Set appropriate log levels for different environments
6. **Monitor Performance**: Debug output can impact performance when enabled

---

## 🔗 Related Files

- **Core Implementation**: `shared/logging.c`, `shared/logging.h`
- **Platform Specific**: `posix/logging.c`, `classic_mac/logging.c`
- **Filtering Script**: `filter_logs.sh`
- **Configuration**: `CLAUDE.md` (logging section)