# Perform Test Feature - Detailed Implementation Plan

## Overview
Add automated testing capability to all three builds (MacTCP, OpenTransport, POSIX) that performs standardized broadcast and direct message tests, with detailed logging to help verify message delivery.

**CRITICAL DESIGN PRINCIPLE:** The test feature **must reuse existing application code** for sending messages. It does NOT create a separate/parallel messaging path. The test calls the same `BroadcastMessage()`, `SendDirectMessageToIP()`, and other messaging functions that the UI uses. This ensures:
- Tests validate the actual production code paths
- No duplicate/divergent messaging implementations
- Changes to messaging logic automatically apply to tests
- Test results accurately reflect real application behavior

## Goals
1. Add "Perform Test" menu item to Classic Mac builds (already in csend.r)
2. Add `/test` command to POSIX build
3. Implement shared test logic that works across all platforms
4. **Reuse ALL existing messaging code** - no separate send paths
5. Generate clear START/END markers in log files
6. Test both broadcast and direct messaging to all peers
7. Include test message IDs for easy verification
8. Update CLAUDE.md to document test feature maintenance requirements

## Architecture

### Shared Test Logic (NEW FILE: shared/test.h, shared/test.c)

Create platform-independent test functions that can be called from any platform:

```c
/* shared/test.h */
#ifndef TEST_H
#define TEST_H

#include "peer.h"

/* Test configuration */
typedef struct {
    int broadcast_count;      /* Number of broadcasts per round (default: 2) */
    int direct_per_peer;      /* Messages per peer per round (default: 2) */
    int test_rounds;          /* Number of test rounds (default: 2) */
    int delay_ms;             /* Delay between messages in ms (default: 4000) */
} test_config_t;

/* Test callbacks - platform provides these */
typedef struct {
    /* Send a broadcast message */
    int (*send_broadcast)(const char *message, void *context);

    /* Send direct message to specific peer IP */
    int (*send_direct)(const char *peer_ip, const char *message, void *context);

    /* Get active peer count */
    int (*get_peer_count)(void *context);

    /* Get peer by index (0-based) */
    int (*get_peer_by_index)(int index, peer_t *out_peer, void *context);

    /* Delay function (platform-specific) */
    void (*delay_func)(int milliseconds, void *context);

    /* Platform-specific context */
    void *context;
} test_callbacks_t;

/* Run automated test sequence */
int run_automated_test(const test_config_t *config, const test_callbacks_t *callbacks);

/* Get default test configuration */
test_config_t get_default_test_config(void);

#endif /* TEST_H */
```

```c
/* shared/test.c */
#include "test.h"
#include "logging.h"
#include "protocol.h"
#include <stdio.h>
#include <string.h>

test_config_t get_default_test_config(void)
{
    test_config_t config;
    config.broadcast_count = 2;
    config.direct_per_peer = 2;
    config.test_rounds = 2;
    config.delay_ms = 4000;  /* 4 seconds between messages */
    return config;
}

int run_automated_test(const test_config_t *config, const test_callbacks_t *callbacks)
{
    int round, i, j;
    int peer_count;
    peer_t peer;
    char message[BUFFER_SIZE];
    int total_messages = 0;
    int failed_messages = 0;

    if (!config || !callbacks) {
        log_error_cat(LOG_CAT_APP_EVENT, "Test: Invalid config or callbacks");
        return -1;
    }

    /* Log test start with clear marker */
    log_app_event("========================================");
    log_app_event("AUTOMATED TEST START");
    log_app_event("Configuration: rounds=%d, broadcasts_per_round=%d, direct_per_peer=%d, delay=%dms",
                  config->test_rounds, config->broadcast_count, config->direct_per_peer, config->delay_ms);

    peer_count = callbacks->get_peer_count(callbacks->context);
    log_app_event("Test: Found %d active peer(s)", peer_count);

    if (peer_count == 0) {
        log_app_event("Test: No peers available - test aborted");
        log_app_event("AUTOMATED TEST END (ABORTED - NO PEERS)");
        log_app_event("========================================");
        return 0;
    }

    /* Run test rounds: each round does broadcasts then directs */
    for (round = 0; round < config->test_rounds; round++) {
        log_app_event("----------------------------------------");
        log_app_event("Test Round %d/%d START", round + 1, config->test_rounds);
        log_app_event("----------------------------------------");

        /* Phase 1 of this round: Broadcast messages */
        log_app_event("Test Round %d - Phase 1: Sending %d broadcast message(s)",
                      round + 1, config->broadcast_count);

        for (i = 0; i < config->broadcast_count; i++) {
            snprintf(message, sizeof(message), "TEST_R%d_BROADCAST_%d", round + 1, i + 1);

            log_app_event("Test Round %d: Broadcasting message %d/%d: '%s'",
                          round + 1, i + 1, config->broadcast_count, message);

            if (callbacks->send_broadcast(message, callbacks->context) != 0) {
                log_error_cat(LOG_CAT_APP_EVENT, "Test Round %d: Broadcast %d FAILED", round + 1, i + 1);
                failed_messages++;
            } else {
                log_app_event("Test Round %d: Broadcast %d sent successfully", round + 1, i + 1);
            }

            total_messages++;

            /* Delay between messages */
            if (i < config->broadcast_count - 1) {
                callbacks->delay_func(config->delay_ms, callbacks->context);
            }
        }

        /* Delay before direct messages */
        callbacks->delay_func(config->delay_ms, callbacks->context);

        /* Phase 2 of this round: Direct messages to each peer */
        log_app_event("Test Round %d - Phase 2: Sending %d direct message(s) to each peer",
                      round + 1, config->direct_per_peer);

        for (i = 0; i < peer_count; i++) {
            if (callbacks->get_peer_by_index(i, &peer, callbacks->context) != 0) {
                log_error_cat(LOG_CAT_APP_EVENT, "Test Round %d: Failed to get peer %d", round + 1, i);
                continue;
            }

            log_app_event("Test Round %d: Sending to peer %d: %s@%s",
                          round + 1, i + 1, peer.username, peer.ip);

            for (j = 0; j < config->direct_per_peer; j++) {
                snprintf(message, sizeof(message), "TEST_R%d_DIRECT_%d_TO_%s_MSG_%d",
                         round + 1, i + 1, peer.username, j + 1);

                log_app_event("Test Round %d: Direct message %d/%d to %s: '%s'",
                              round + 1, j + 1, config->direct_per_peer, peer.username, message);

                if (callbacks->send_direct(peer.ip, message, callbacks->context) != 0) {
                    log_error_cat(LOG_CAT_APP_EVENT, "Test Round %d: Direct message to %s FAILED",
                                  round + 1, peer.username);
                    failed_messages++;
                } else {
                    log_app_event("Test Round %d: Direct message to %s sent successfully",
                                  round + 1, peer.username);
                }

                total_messages++;

                /* Delay between messages */
                if (j < config->direct_per_peer - 1) {
                    callbacks->delay_func(config->delay_ms, callbacks->context);
                }
            }

            /* Delay before next peer */
            if (i < peer_count - 1) {
                callbacks->delay_func(config->delay_ms, callbacks->context);
            }
        }

        log_app_event("----------------------------------------");
        log_app_event("Test Round %d/%d COMPLETE", round + 1, config->test_rounds);
        log_app_event("----------------------------------------");

        /* Delay before next round */
        if (round < config->test_rounds - 1) {
            callbacks->delay_func(config->delay_ms * 2, callbacks->context);
        }
    }

    /* Log test completion */
    log_app_event("========================================");
    log_app_event("AUTOMATED TEST END");
    log_app_event("Test Summary: %d total messages, %d failed, %d succeeded",
                  total_messages, failed_messages, total_messages - failed_messages);
    log_app_event("========================================");

    return failed_messages == 0 ? 0 : -1;
}
```

## Platform-Specific Implementations

### 1. OpenTransport Build (classic_mac_ot/)

#### 1.1 Update main.c

Add constant for test menu item:
```c
/* Line 48, after kQuitItem */
#define kPerformTestItem 1  /* First item in File menu */
```

Update HandleMenuChoice to handle test:
```c
/* Line 265-269, in kFileMenuID case */
case kFileMenuID:
    if (menuItem == kPerformTestItem) {
        log_app_event("HandleMenuChoice: File->Perform Test selected");
        PerformAutomatedTest();  /* New function */
    } else if (menuItem == kQuitItem) {
        log_app_event("HandleMenuChoice: File->Quit selected by user. Setting gDone=true.");
        gDone = true;
    }
    break;
```

Note: The menu resource already has "Perform Test" as item 1 and "Quit" as item 2 in menu 128 (File menu).

#### 1.2 Create classic_mac_ot/test.c

```c
#include "test.h"
#include "../shared/test.h"
#include "../shared/logging.h"
#include "../shared/peer_wrapper.h"
#include "messaging.h"
#include <Gestalt.h>
#include <Timer.h>

/* Delay function for Classic Mac */
static void mac_delay_ms(int milliseconds, void *context)
{
    unsigned long finalTicks;
    (void)context;

    /* Convert milliseconds to ticks (1 tick = ~16.67ms) */
    Delay((milliseconds * 60) / 1000, &finalTicks);
}

/* Broadcast callback - REUSES EXISTING APPLICATION CODE */
static int test_send_broadcast(const char *message, void *context)
{
    (void)context;
    /* Calls the SAME function the UI uses for broadcasts */
    return (BroadcastMessage(message) == noErr) ? 0 : -1;
}

/* Direct message callback - REUSES EXISTING APPLICATION CODE */
static int test_send_direct(const char *peer_ip, const char *message, void *context)
{
    (void)context;
    /* Calls the SAME function the UI uses for direct messages */
    return (SendDirectMessageToIP(message, peer_ip) == noErr) ? 0 : -1;
}

/* Get peer count callback */
static int test_get_peer_count(void *context)
{
    (void)context;
    return pw_get_active_peer_count();
}

/* Get peer by index callback */
static int test_get_peer_by_index(int index, peer_t *out_peer, void *context)
{
    (void)context;
    pw_get_peer_by_index(index, out_peer);
    return 0;
}

void PerformAutomatedTest(void)
{
    test_config_t config;
    test_callbacks_t callbacks;

    log_app_event("PerformAutomatedTest: Starting automated test");

    /* Get default config */
    config = get_default_test_config();

    /* Set up callbacks */
    callbacks.send_broadcast = test_send_broadcast;
    callbacks.send_direct = test_send_direct;
    callbacks.get_peer_count = test_get_peer_count;
    callbacks.get_peer_by_index = test_get_peer_by_index;
    callbacks.delay_func = mac_delay_ms;
    callbacks.context = NULL;

    /* Run test */
    run_automated_test(&config, &callbacks);

    log_app_event("PerformAutomatedTest: Test completed");
}
```

#### 1.3 Create classic_mac_ot/test.h

```c
#ifndef TEST_OT_H
#define TEST_OT_H

/* Run automated test from menu selection */
void PerformAutomatedTest(void);

#endif /* TEST_OT_H */
```

#### 1.4 Add SendDirectMessageToIP to messaging.c (IF NOT ALREADY PRESENT)

**IMPORTANT:** First check if a similar function already exists in messaging.c. If there's already a function to send direct messages by IP, **USE THAT** instead of creating a new one. The goal is to reuse existing code.

If no such function exists, add this wrapper:

```c
/* Add to messaging.c - wrapper around existing TCP send logic */
OSErr SendDirectMessageToIP(const char* message, const char* targetIP)
{
    char formattedMessage[BUFFER_SIZE];
    int formatted_len;

    if (!message || !targetIP) {
        log_error_cat(LOG_CAT_MESSAGING, "SendDirectMessageToIP: Invalid parameters");
        return paramErr;
    }

    /* Uses the SAME format_message() and SendTCPMessage() functions used elsewhere */
    formatted_len = format_message(formattedMessage, sizeof(formattedMessage),
                                   MSG_TYPE_TEXT,
                                   generate_message_id(),
                                   GetMyUsername(), GetMyLocalIP(), message);

    if (formatted_len < 0) {
        log_error_cat(LOG_CAT_MESSAGING, "SendDirectMessageToIP: format_message failed");
        return paramErr;
    }

    /* Calls the existing TCP send function - NO new networking code */
    return SendTCPMessage(formattedMessage, targetIP, TCP_PORT);
}
```

Add to messaging.h:
```c
OSErr SendDirectMessageToIP(const char* message, const char* targetIP);
```

### 2. MacTCP Build (classic_mac_mactcp/)

Same structure as OpenTransport, with platform-specific implementations for MacTCP APIs.

Files to create/modify:
- `classic_mac_mactcp/test.c` - same as OT but uses MacTCP messaging functions
- `classic_mac_mactcp/test.h` - same as OT
- `classic_mac_mactcp/main.c` - add kPerformTestItem and menu handling
- `classic_mac_mactcp/messaging.c` - add SendDirectMessageToIP if not present

### 3. POSIX Build (posix/)

#### 3.1 Add command handler to commands.c

```c
/* Add to commands.c */
int handle_test_command(app_state_t *state, const char *args)
{
    (void)args; /* Unused for now */

    log_app_event("Executing /test command");
    run_posix_automated_test(state);

    return 0;
}
```

#### 3.2 Create posix/test.c

```c
#include "test.h"
#include "../shared/test.h"
#include "../shared/logging.h"
#include "../shared/peer_wrapper.h"
#include "messaging.h"
#include "app_state.h"
#include <unistd.h>
#include <time.h>

/* Delay function for POSIX */
static void posix_delay_ms(int milliseconds, void *context)
{
    (void)context;
    usleep(milliseconds * 1000);
}

/* Broadcast callback - REUSES EXISTING APPLICATION CODE */
static int test_send_broadcast(const char *message, void *context)
{
    app_state_t *state = (app_state_t *)context;
    /* Calls the SAME function the UI uses for broadcasts */
    return broadcast_message(state, message);
}

/* Direct message callback - REUSES EXISTING APPLICATION CODE */
static int test_send_direct(const char *peer_ip, const char *message, void *context)
{
    app_state_t *state = (app_state_t *)context;
    /* Calls the SAME function the UI uses for direct messages */
    return send_direct_message_to_ip(state, peer_ip, message);
}

/* Get peer count callback */
static int test_get_peer_count(void *context)
{
    (void)context;
    return pw_get_active_peer_count();
}

/* Get peer by index callback */
static int test_get_peer_by_index(int index, peer_t *out_peer, void *context)
{
    (void)context;
    pw_get_peer_by_index(index, out_peer);
    return 0;
}

void run_posix_automated_test(app_state_t *state)
{
    test_config_t config;
    test_callbacks_t callbacks;

    if (!state) {
        log_error_cat(LOG_CAT_APP_EVENT, "Test: Invalid state");
        return;
    }

    log_app_event("Starting automated test from /test command");

    /* Get default config */
    config = get_default_test_config();

    /* Set up callbacks */
    callbacks.send_broadcast = test_send_broadcast;
    callbacks.send_direct = test_send_direct;
    callbacks.get_peer_count = test_get_peer_count;
    callbacks.get_peer_by_index = test_get_peer_by_index;
    callbacks.delay_func = posix_delay_ms;
    callbacks.context = state;

    /* Run test */
    run_automated_test(&config, &callbacks);

    log_app_event("Automated test from /test command completed");
}
```

#### 3.3 Create posix/test.h

```c
#ifndef TEST_POSIX_H
#define TEST_POSIX_H

#include "app_state.h"

/* Run automated test from /test command */
void run_posix_automated_test(app_state_t *state);

#endif /* TEST_POSIX_H */
```

#### 3.4 Add send_direct_message_to_ip to messaging.c (IF NOT ALREADY PRESENT)

**IMPORTANT:** First check if a similar function already exists in posix/messaging.c. If there's already a function to send direct messages by IP, **USE THAT** instead of creating a new one. The goal is to reuse existing code.

If no such function exists, add this wrapper:

```c
/* Add to posix/messaging.c - wrapper around existing TCP send logic */
int send_direct_message_to_ip(app_state_t *state, const char *peer_ip, const char *message)
{
    char formatted_msg[BUFFER_SIZE];
    int formatted_len;

    if (!state || !peer_ip || !message) {
        log_error_cat(LOG_CAT_MESSAGING, "Invalid parameters");
        return -1;
    }

    /* Uses the SAME format_message() function used elsewhere */
    formatted_len = format_message(formatted_msg, sizeof(formatted_msg),
                                   MSG_TYPE_TEXT,
                                   generate_message_id(),
                                   state->username, state->local_ip, message);

    if (formatted_len < 0) {
        return -1;
    }

    /* Calls the existing TCP send function - NO new networking code */
    return send_tcp_message(state, peer_ip, TCP_PORT, formatted_msg);
}
```

Add to messaging.h:
```c
int send_direct_message_to_ip(app_state_t *state, const char *peer_ip, const char *message);
```

#### 3.5 Update command table in ui_terminal.c

```c
/* Add to command_table array in posix/ui_terminal.c */
{"/test", handle_test_command, "Run automated test sequence"},
```

## Build System Updates

### Makefile Changes (posix/Makefile)

Add shared/test.c and posix/test.c to compilation:

```makefile
SHARED_SRCS = ... shared/test.c
POSIX_SRCS = ... posix/test.c
```

### Makefile.retro68.ot Changes

Add shared/test.c and classic_mac_ot/test.c:

```makefile
SHARED_SRCS = ... shared/test.c
OT_SRCS = ... classic_mac_ot/test.c
```

### Makefile.retro68.mactcp Changes

Add shared/test.c and classic_mac_mactcp/test.c:

```makefile
SHARED_SRCS = ... shared/test.c
MACTCP_SRCS = ... classic_mac_mactcp/test.c
```

## Menu Resource (Already Done)

The menu resource in `MPW_resources/csend.r` already contains:
- Menu 128 (File menu) with:
  - Item 1: "Perform Test" (command-T)
  - Item 2: "Quit" (command-Q)

No changes needed to the resource file.

## Testing Procedure

### 1. Build all three versions
```bash
make clean && make                                    # POSIX
make -f Makefile.retro68.ot clean && make -f Makefile.retro68.ot      # OpenTransport
make -f Makefile.retro68.mactcp clean && make -f Makefile.retro68.mactcp  # MacTCP
```

### 2. Test POSIX version
```bash
./build/posix/csend_posix user1
# In another terminal
./build/posix/csend_posix user2
# In user1 terminal, type:
/test
# Check csend_posix.log for test markers
```

### 3. Test Classic Mac versions
- Run OpenTransport build
- Run MacTCP build
- Run POSIX build
- On OpenTransport Mac: Select "File → Perform Test"
- Check csend_classic_mac_ot_ppc.log for test markers
- Verify messages appear on all peers

## Expected Log Output

```
[timestamp] [APP_EVENT] ========================================
[timestamp] [APP_EVENT] AUTOMATED TEST START
[timestamp] [APP_EVENT] Configuration: rounds=2, broadcasts_per_round=2, direct_per_peer=2, delay=4000ms
[timestamp] [APP_EVENT] Test: Found 2 active peer(s)
[timestamp] [APP_EVENT] ----------------------------------------
[timestamp] [APP_EVENT] Test Round 1/2 START
[timestamp] [APP_EVENT] ----------------------------------------
[timestamp] [APP_EVENT] Test Round 1 - Phase 1: Sending 2 broadcast message(s)
[timestamp] [APP_EVENT] Test Round 1: Broadcasting message 1/2: 'TEST_R1_BROADCAST_1'
[timestamp] [DEBUG][MESSAGING] Broadcasting message: TEST_R1_BROADCAST_1
[timestamp] [DEBUG][MESSAGING] Sending TCP message (TEXT) to 10.188.1.19: TEST_R1_BROADCAST_1
[timestamp] [APP_EVENT] Test Round 1: Broadcast 1 sent successfully
[4 second delay]
[timestamp] [APP_EVENT] Test Round 1: Broadcasting message 2/2: 'TEST_R1_BROADCAST_2'
[timestamp] [APP_EVENT] Test Round 1: Broadcast 2 sent successfully
[4 second delay]
[timestamp] [APP_EVENT] Test Round 1 - Phase 2: Sending 2 direct message(s) to each peer
[timestamp] [APP_EVENT] Test Round 1: Sending to peer 1: posix@10.188.1.19
[timestamp] [APP_EVENT] Test Round 1: Direct message 1/2 to posix: 'TEST_R1_DIRECT_1_TO_posix_MSG_1'
[timestamp] [APP_EVENT] Test Round 1: Direct message to posix sent successfully
[4 second delay]
[timestamp] [APP_EVENT] Test Round 1: Direct message 2/2 to posix: 'TEST_R1_DIRECT_1_TO_posix_MSG_2'
[4 second delay]
[timestamp] [APP_EVENT] Test Round 1: Sending to peer 2: mactcp@10.188.1.213
[timestamp] [APP_EVENT] Test Round 1: Direct message 1/2 to mactcp: 'TEST_R1_DIRECT_2_TO_mactcp_MSG_1'
...
[timestamp] [APP_EVENT] ----------------------------------------
[timestamp] [APP_EVENT] Test Round 1/2 COMPLETE
[timestamp] [APP_EVENT] ----------------------------------------
[8 second delay before next round]
[timestamp] [APP_EVENT] ----------------------------------------
[timestamp] [APP_EVENT] Test Round 2/2 START
[timestamp] [APP_EVENT] ----------------------------------------
[timestamp] [APP_EVENT] Test Round 2 - Phase 1: Sending 2 broadcast message(s)
[timestamp] [APP_EVENT] Test Round 2: Broadcasting message 1/2: 'TEST_R2_BROADCAST_1'
...
[timestamp] [APP_EVENT] ----------------------------------------
[timestamp] [APP_EVENT] Test Round 2/2 COMPLETE
[timestamp] [APP_EVENT] ----------------------------------------
[timestamp] [APP_EVENT] ========================================
[timestamp] [APP_EVENT] AUTOMATED TEST END
[timestamp] [APP_EVENT] Test Summary: 16 total messages, 0 failed, 16 succeeded
[timestamp] [APP_EVENT] ========================================

Total messages with 2 peers: 2 rounds × (2 broadcasts + 2 peers × 2 directs) = 2 × (2 + 4) = 12 messages
```

## Implementation Order

1. ✅ Create shared/test.h and shared/test.c (core logic)
2. ✅ Implement OpenTransport version (classic_mac_ot/test.c, update main.c)
3. ✅ Implement MacTCP version (classic_mac_mactcp/test.c, update main.c)
4. ✅ Implement POSIX version (posix/test.c, update commands.c and ui_terminal.c)
5. ✅ Update all Makefiles
6. ✅ Build and test each version
7. ✅ Verify log output format
8. ✅ Test cross-platform (all three builds running simultaneously)
9. ✅ **Update CLAUDE.md** to document test feature maintenance requirements

## CLAUDE.md Updates

After implementation, add the following section to CLAUDE.md:

```markdown
## Automated Test Feature

### Overview
The application includes an automated test feature accessible via:
- **Classic Mac builds** (MacTCP & OpenTransport): `File → Perform Test` menu
- **POSIX build**: `/test` command

### Critical Maintenance Requirement

**⚠️ The test feature REUSES existing application messaging code. It does NOT have separate send logic.**

When modifying messaging functions, ensure changes apply correctly to test usage:
- `BroadcastMessage()` / `broadcast_message()` - Used by test for broadcasts
- `SendDirectMessageToIP()` / `send_direct_message_to_ip()` - Used by test for direct messages
- `SendTCPMessage()` / `send_tcp_message()` - Underlying TCP send function
- `format_message()` - Message formatting used by test

### Test Implementation Files
- `shared/test.c` - Platform-independent test logic with callback pattern
- `shared/test.h` - Test configuration and callback interface
- `classic_mac_ot/test.c` - OpenTransport test adapter
- `classic_mac_mactcp/test.c` - MacTCP test adapter
- `posix/test.c` - POSIX test adapter
- `posix/commands.c` - `/test` command handler

### Test Configuration
Default test parameters (in `get_default_test_config()`):
- 2 test rounds
- 2 broadcasts per round
- 2 direct messages to each peer per round
- 4 second delay between messages

### Adding New Messaging Features
If adding new message types or send functions:
1. Implement the feature in the normal messaging code
2. Consider if tests should use the new feature
3. If yes, update test callbacks in platform-specific test.c files
4. Maintain the principle: **tests call the same code the UI uses**

### Running Tests
Tests generate clear log markers:
```
[APP_EVENT] ========================================
[APP_EVENT] AUTOMATED TEST START
...
[APP_EVENT] AUTOMATED TEST END
[APP_EVENT] ========================================
```

Test messages are clearly labeled: `TEST_R1_BROADCAST_1`, `TEST_R2_DIRECT_1_TO_peer_MSG_1`
```

## Benefits

1. **Automated Testing**: No manual typing required
2. **Reproducible**: Same test sequence every time
3. **Verifiable**: Clear START/END markers and message IDs
4. **Cross-Platform**: Works on all three builds
5. **Debugging**: Detailed logging helps identify message loss
6. **Documentation**: Test messages clearly labeled for easy identification
7. **Code Reuse**: Tests validate actual production code paths, not mock implementations
8. **Maintenance**: Changes to messaging automatically apply to tests

## References

- Inside Macintosh: Macintosh Toolbox Essentials (Menu Manager)
- Inside Macintosh: Operating System Utilities (Delay function)
- NetworkingOpenTransport.txt (for OT-specific timing considerations)
